/*
 * hook.c —— 见 hook.h。
 */
#include "hook.h"
#include "hua.h"
#include "platform.h"
#include "recognizer.h"
#include "watchdog.h"

#include <string.h>

/* HOOK_MAX_PTS 现由 hook.h 导出（overlay 需要它做编译期断言）。 */

typedef enum { ST_IDLE = 0, ST_TENTATIVE, ST_ACTIVE } HookState;

static HHOOK      g_hook;
static HWND       g_notify;
static HuaTrigger g_trigger;
static int        g_trigger_dist = 5;    /* 进入 Active 的阈值 */
static int        g_min_dist = 20;       /* 方向分段阈值 */
static int        g_step_dist = 12;      /* 采点去抖 */

static HookGateFn g_gate;
static HookState  g_state;
static ULONGLONG  g_last_input_tick;
/* 探活用：最近一次回调时间，与上次探活时的光标位置/时刻。 */
static ULONGLONG  g_last_cb_tick;
static Pt         g_wd_pos;
static ULONGLONG  g_wd_tick;
static WatchdogState g_wd_state;   /* 判定状态，逻辑在 watchdog.c（有单测） */
static bool       g_suppress_trigger_up;
/* 抑制那个 Up 的同时，还要在它到来时补发一次原生点击（见 hook_arm_replay_on_up）。
 * 只有 g_suppress_trigger_up 为真时才有意义。 */
static bool       g_replay_on_up;
static Pt         g_pts[HOOK_MAX_PTS];
static size_t     g_npts;
static Pt         g_start;
static Pt         g_last_pos;
static HWND       g_target;
static HWND       g_foreground_at_down;
static char       g_seq[REC_MAX_SEQ];   /* 最近识别到的方向串 */

/* ---------------- 触发键判定 ---------------- */

static bool is_trigger_down(WPARAM msg, const MSLLHOOKSTRUCT *ms)
{
    switch (g_trigger) {
    case HUA_TRIGGER_RIGHT:  return msg == WM_RBUTTONDOWN;
    case HUA_TRIGGER_MIDDLE: return msg == WM_MBUTTONDOWN;
    case HUA_TRIGGER_X1:
        return msg == WM_XBUTTONDOWN && GET_XBUTTON_WPARAM(ms->mouseData) == XBUTTON1;
    case HUA_TRIGGER_X2:
        return msg == WM_XBUTTONDOWN && GET_XBUTTON_WPARAM(ms->mouseData) == XBUTTON2;
    }
    return false;
}

static bool is_trigger_up(WPARAM msg, const MSLLHOOKSTRUCT *ms)
{
    switch (g_trigger) {
    case HUA_TRIGGER_RIGHT:  return msg == WM_RBUTTONUP;
    case HUA_TRIGGER_MIDDLE: return msg == WM_MBUTTONUP;
    case HUA_TRIGGER_X1:
        return msg == WM_XBUTTONUP && GET_XBUTTON_WPARAM(ms->mouseData) == XBUTTON1;
    case HUA_TRIGGER_X2:
        return msg == WM_XBUTTONUP && GET_XBUTTON_WPARAM(ms->mouseData) == XBUTTON2;
    }
    return false;
}

/* ---------------- 采点与补发 ---------------- */

static void add_point(Pt p)
{
    if (g_npts < HOOK_MAX_PTS)
        g_pts[g_npts++] = p;
}

/* 钩子 Move 与主线程轮询共用同一套采点/激活逻辑。 */
static void track_point(Pt p)
{
    if (g_state != ST_TENTATIVE && g_state != ST_ACTIVE)
        return;

    if (p.x != g_last_pos.x || p.y != g_last_pos.y) {
        g_last_pos = p;
        g_last_input_tick = GetTickCount64();
    }

    /* 用 64 位做平方：long 在 Windows（LLP64）恒为 32 位，坐标差的平方和在超宽
     * 虚拟桌面上可能溢出（有符号溢出是 UB）。阈值侧已由 config_clamp 挡住，
     * 但坐标侧取决于光标位置，不受配置约束。 */
    Pt last = g_pts[g_npts - 1];
    long long dx = p.x - last.x, dy = p.y - last.y;
    if (dx * dx + dy * dy >= (long long)g_step_dist * g_step_dist)
        add_point(p);

    if (g_state == ST_TENTATIVE) {
        long long sx = p.x - g_start.x, sy = p.y - g_start.y;
        if (sx * sx + sy * sy >= (long long)g_trigger_dist * g_trigger_dist)
            g_state = ST_ACTIVE;
    }
}

/* 补发一次原生触发键点击（下+上），带签名以便自身钩子放行。
 * 用当前光标位置（点击时几乎未移动，即原始坐标）。 */
static void synth_click(HuaTrigger t)
{
    INPUT in[2];
    memset(in, 0, sizeof(in));
    DWORD down = 0, up = 0, data = 0;
    switch (t) {
    case HUA_TRIGGER_RIGHT:  down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP;  break;
    case HUA_TRIGGER_MIDDLE: down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; break;
    case HUA_TRIGGER_X1:     down = MOUSEEVENTF_XDOWN; up = MOUSEEVENTF_XUP; data = XBUTTON1; break;
    case HUA_TRIGGER_X2:     down = MOUSEEVENTF_XDOWN; up = MOUSEEVENTF_XUP; data = XBUTTON2; break;
    }
    /* 不写 default：那会压制 MSVC 的 C4062（/W4 默认开启），使得将来新增触发键时
     * 这里悄无声息，而 is_trigger_down/is_trigger_up 会照常告警——补发路径静默失效
     * 比编译告警难查得多。未覆盖的枚举值落到这里，按「无法补发」处理。 */
    if (!down)
        return;
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = down;
    in[0].mi.mouseData = data;
    in[0].mi.dwExtraInfo = HUA_SIGNATURE;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = up;
    in[1].mi.mouseData = data;
    in[1].mi.dwExtraInfo = HUA_SIGNATURE;
    UINT sent = SendInput(2, in, sizeof(INPUT));
    if (sent != 2) {
        /* 只注入了 Down 会让目标程序认为触发键一直按着。补发 Up 收拾干净。 */
        if (sent == 1)
            SendInput(1, &in[1], sizeof(INPUT));
        HUA_LOG_W("补发触发键点击失败：仅注入 %u/2 个事件", sent);
    }
}

/* ---------------- 事件处理 ---------------- */
/* 返回 1=吞掉本事件；-1=放行。 */

/*
 * 锁定手势起点下方的顶层拥有者。不能使用 GetForegroundWindow：触发键
 * Down 会被钩子扣住，光标下方的非活动窗口不会先获得焦点。
 * GA_ROOTOWNER 与 StrokesPlus 的目标选择一致，也能把子控件/临时弹窗归到
 * 应当接收窗口动作的顶层窗口。命中失败时才退回当前前台窗口。
 */
static HWND target_from_point(POINT p)
{
    HWND hit = WindowFromPoint(p);
    HWND target = hit ? GetAncestor(hit, GA_ROOTOWNER) : NULL;
    if (!target)
        target = hit;
    if (!target)
        target = GetForegroundWindow();
    return target;
}

static LRESULT handle_event(WPARAM msg, const MSLLHOOKSTRUCT *ms)
{
    Pt p = { (int)ms->pt.x, (int)ms->pt.y };

    if (is_trigger_down(msg, ms)) {
        /* 新一轮输入开始；清除上一轮因外部工具吞 Up 留下的抑制标记。 */
        g_suppress_trigger_up = false;
        g_replay_on_up = false;
        HWND target = target_from_point(ms->pt);
        /* 门控和后续动作始终使用起点下方的同一个目标窗口。 */
        if (g_gate && !g_gate(target))
            return -1;
        g_state  = ST_TENTATIVE;
        g_last_input_tick = GetTickCount64();
        g_start  = p;
        g_last_pos = p;
        g_target = target;   /* 此刻锁定，不能等到 Up 或再读前台窗口 */
        g_foreground_at_down = GetForegroundWindow();
        g_npts   = 0;
        add_point(p);
        /* 成熟手势工具均从 Down 开始轮询；轨迹仍要过移动阈值才真正显示。 */
        PostMessage(g_notify, WM_HUA_HOOK, HUA_EV_GESTURE_BEGIN, 0);
        return 1;   /* 先扣住 Down，不放行 */
    }

    if (msg == WM_MOUSEMOVE) {
        track_point(p);
        return -1;   /* 移动事件从不吞 */
    }

    if (is_trigger_up(msg, ms)) {
        /* 轨迹超时后仍要吞掉迟到的物理 Up，绝不把它泄漏给目标程序：Down 早已被吞，
         * 放行一个孤儿 Up 只会让目标程序收到没有配对 Down 的事件。 */
        if (g_suppress_trigger_up) {
            bool replay = g_replay_on_up;
            g_suppress_trigger_up = false;
            g_replay_on_up = false;
            /* 超时时判定「根本没划出手势」→ 这次就是一次普通点击，此刻（用户真正
             * 松手的时刻）补发原生点击，菜单便和平时一样在松手时弹出。补发本身走
             * 主线程，绝不在 LL 回调里调 SendInput。 */
            if (replay)
                PostMessage(g_notify, WM_HUA_HOOK, HUA_EV_GESTURE_CANCEL, 0);
            return 1;
        }

        /* Up 自带最终坐标；即使中间 Move 丢失也能完成激活和末点采样。 */
        track_point(p);
        if (g_state == ST_TENTATIVE) {
            /* 仅点击、未成手势：主线程异步补发，避免在 LL Hook 内调用 SendInput。 */
            g_state = ST_IDLE;
            PostMessage(g_notify, WM_HUA_HOOK, HUA_EV_GESTURE_CANCEL, 0);
            return 1;
        }
        if (g_state == ST_ACTIVE) {
            /* 成手势 → 编码方向串，交主线程匹配执行，吞掉 Up。 */
            g_state = ST_IDLE;
            rec_encode(g_pts, g_npts, g_min_dist, g_seq, sizeof(g_seq));
            PostMessage(g_notify, WM_HUA_HOOK, HUA_EV_GESTURE_END, 0);
            return 1;
        }
    }

    return -1;
}

static LRESULT CALLBACK low_level_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        g_last_cb_tick = GetTickCount64();   /* 探活心跳：证明钩子仍在派发 */
        const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lParam;
        /* 放行自身补发的合成事件（LLMHF_INJECTED + 签名），不参与状态机。 */
        bool self = (ms->flags & LLMHF_INJECTED) && (ms->dwExtraInfo == HUA_SIGNATURE);
        if (!self) {
            LRESULT r = handle_event(wParam, ms);
            if (r >= 0)
                return r;   /* 吞掉 */
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

/* ---------------- 安装 / 卸载 / 访问器 ---------------- */

bool hook_install(HWND notify_hwnd, HuaTrigger trigger,
                  int trigger_dist, int min_dist, int step_dist)
{
    if (g_hook)
        return true;
    g_notify      = notify_hwnd;
    g_trigger     = trigger;
    g_trigger_dist = trigger_dist > 0 ? trigger_dist : 5;
    g_min_dist    = min_dist > 0 ? min_dist : 20;
    g_step_dist   = step_dist > 0 ? step_dist : 12;
    g_state       = ST_IDLE;
    g_suppress_trigger_up = false;
    g_replay_on_up = false;
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, low_level_mouse_proc,
                               GetModuleHandleW(NULL), 0);
    return g_hook != NULL;
}

void hook_uninstall(void)
{
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    g_notify = NULL;
    g_state  = ST_IDLE;
    g_suppress_trigger_up = false;
    g_replay_on_up = false;
    /* 必须一并清空采样点：否则重装后主线程若仍在跑 TIMER_FRAME，
     * hook_snapshot() 会返回上一轮的陈旧轨迹，浮层会把它永久画在屏幕上。 */
    g_npts   = 0;
}

bool hook_is_idle(void) { return g_state == ST_IDLE; }

/*
 * 钩子探活。系统在回调超时后会静默摘钩且不通知，g_hook 仍是陈旧的非空句柄，
 * 而 Win32 没有「查询钩子是否还在」的 API。故用启发式：若上个探活周期内光标
 * 明显移动过，钩子却一个事件都没收到，即判定已被摘。
 * 返回 true 表示需要（重）安装。
 */
bool hook_looks_dead(void)
{
    /* 本函数只负责采样，判定交给 watchdog_should_reinstall（纯逻辑、有单测）。
     * 这个判定曾两次写错且都逃过肉眼 review，故把它挪出 Win32 代码。 */
    POINT p;
    ULONGLONG now = GetTickCount64();

    WatchdogSample s;
    s.installed    = (g_hook != NULL);
    s.cursor_ok    = GetCursorPos(&p) ? true : false;
    s.moved        = false;
    s.last_cb_tick = g_last_cb_tick;
    s.prev_tick    = g_wd_tick;

    if (s.cursor_ok) {
        s.moved = (p.x != g_wd_pos.x || p.y != g_wd_pos.y);
        g_wd_pos.x = (int)p.x;
        g_wd_pos.y = (int)p.y;
    }
    g_wd_tick = now;   /* 无论成败都推进基线，避免下周期拿过期值比较 */

    return watchdog_should_reinstall(&g_wd_state, &s);
}

const char *hook_last_seq(void)   { return g_seq; }
HWND        hook_last_target(void){ return g_target; }

void hook_poll_cursor(void)
{
    if (g_state == ST_TENTATIVE || g_state == ST_ACTIVE) {
        POINT p;
        if (GetCursorPos(&p)) {
            Pt current = { (int)p.x, (int)p.y };
            track_point(current);
        }
    }
}

size_t hook_snapshot(const Pt **out)
{
    if (out)
        *out = g_pts;
    return g_npts;
}

bool hook_cancel_if_timed_out(DWORD timeout_ms, HookTimeoutInfo *info)
{
    /*
     * 只兜底 ST_ACTIVE，**不要**把 ST_TENTATIVE 也纳进来。
     *
     * 曾经试图纳入，动机是：UAC 安全桌面/锁屏吞掉 Up 时状态会卡在 TENTATIVE，
     * 用户回来一动鼠标就转入 ACTIVE，出现一条没人按键的「幽灵轨迹」。
     * 但那样会打断一个日常高频操作：按住右键不动想一下再松手。track_point 只在
     * 坐标变化时刷新 g_last_input_tick，手不动就必定判超时 —— 于是 Down 已被吞、
     * Up 又被 g_suppress_trigger_up 吞掉，这次右键凭空消失（已实测复现）。
     *
     * 也无法用 GetAsyncKeyState 区分「按住不动」与「Up 丢了」：Down 正是被我们
     * 自己吞掉的（回调返回 1），系统从未更新按键状态，实测全程读到「未按下」。
     *
     * 而幽灵轨迹是**会自愈**的：它一旦转入 ACTIVE，用户停手 PauseTimeout 后就被
     * 下面这段收掉。用一个会自愈的观感问题去换掉核心交互，不划算。
     */
    if (g_state != ST_ACTIVE || timeout_ms == 0)
        return false;

    ULONGLONG elapsed = GetTickCount64() - g_last_input_tick;
    if (elapsed < timeout_ms)
        return false;

    if (info) {
        memset(info, 0, sizeof(*info));
        info->idle_ms = elapsed;
        info->timeout_ms = timeout_ms;
        info->point_count = g_npts;
        info->start = g_start;
        info->last = g_last_pos;
        POINT cursor;
        if (GetCursorPos(&cursor)) {
            info->cursor.x = (int)cursor.x;
            info->cursor.y = (int)cursor.y;
            info->cursor_valid = true;
        }
        info->target = g_target;
        info->foreground_at_down = g_foreground_at_down;
        info->foreground_at_timeout = GetForegroundWindow();
    }

    g_state = ST_IDLE;
    g_suppress_trigger_up = true;
    return true;
}

void hook_replay_trigger_click(void) { synth_click(g_trigger); }

/*
 * 安排「这次的物理 Up 到来时补发一次原生点击」。
 *
 * 只在 hook_cancel_if_timed_out 返回 true 之后由主线程调用，且仅当那次超时**没有
 * 划出任何方向段**时——那种情况下用户其实只是按住触发键不动想了一下，手抖
 * TriggerDistance(默认 5px) 就足以把状态推进 ST_ACTIVE，于是 Down 被吞、Up 也被
 * g_suppress_trigger_up 吞掉，这次右键凭空消失、菜单永远不弹。
 * （hook_cancel_if_timed_out 上方的注释刻意不把 ST_TENTATIVE 纳入超时正是为了保护
 * 这个操作，但那层保护挡不住 5px 手抖推进来的 ST_ACTIVE。）
 *
 * 为什么不在超时的当下直接补发：此刻按键还**物理按着**，那样菜单会在按下满 1 秒时
 * 突然弹出，而不是在用户松手时。推迟到 Up 才补发，观感与原生右键完全一致。
 *
 * 「要不要补发」是配置决策（RestoreEvent），故留在 main.c 判；hook 只提供机制。
 */
void hook_arm_replay_on_up(void)
{
    if (g_suppress_trigger_up)
        g_replay_on_up = true;
}

void hook_set_gate(HookGateFn fn) { g_gate = fn; }
