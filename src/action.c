/*
 * action.c —— 见 action.h。
 */
#include "action.h"
#include "platform.h"

#include <assert.h>
#include <string.h>
#include <wchar.h>   /* wcscmp：MSVC 经 string.h 也能拿到，MinGW 下未必 */
#include <ctype.h>
#include <stdlib.h>

/* ---------------- 快捷键解析（纯逻辑） ---------------- */

static void str_tolower_inplace(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

/* 键名 → 虚拟键码；未知返回 0。传入的 token 已小写去空格。 */
static WORD key_name_to_vk(const char *t)
{
    size_t len = strlen(t);
    if (len == 0)
        return 0;

    if (len == 1) {
        char c = t[0];
        if (c >= 'a' && c <= 'z') return (WORD)(c - 'a' + 'A');
        if (c >= '0' && c <= '9') return (WORD)c;
        return 0;
    }

    /* 功能键 f1..f24。整串校验：atoi 会忽略尾部垃圾，"f12abc" 也会被当成 F12。 */
    if (t[0] == 'f' && isdigit((unsigned char)t[1])) {
        const char *p = t + 1;
        while (isdigit((unsigned char)*p))
            p++;
        if (*p == '\0') {   /* 数字之后必须就是结尾 */
            int n = atoi(t + 1);
            if (n >= 1 && n <= 24)
                return (WORD)(VK_F1 + (n - 1));
        }
    }

    static const struct { const char *name; WORD vk; } map[] = {
        {"esc", VK_ESCAPE},   {"escape", VK_ESCAPE},
        {"enter", VK_RETURN}, {"return", VK_RETURN},
        {"tab", VK_TAB},      {"space", VK_SPACE},
        {"backspace", VK_BACK}, {"back", VK_BACK},
        {"delete", VK_DELETE}, {"del", VK_DELETE},
        {"insert", VK_INSERT}, {"ins", VK_INSERT},
        {"home", VK_HOME},    {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
        {"pagedown", VK_NEXT},{"pgdn", VK_NEXT},
        {"left", VK_LEFT},    {"right", VK_RIGHT},
        {"up", VK_UP},        {"down", VK_DOWN},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(t, map[i].name) == 0)
            return map[i].vk;

    return 0;
}

bool action_parse_key(const char *spec, KeyCombo *out)
{
    if (out) {
        out->ctrl = out->alt = out->shift = out->win = false;
        out->vk = 0;
    }
    if (!spec || !out)
        return false;

    char buf[128];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    str_tolower_inplace(buf);

    bool has_main = false;
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        /* 去首尾空格 */
        while (*tok == ' ') tok++;
        size_t l = strlen(tok);
        while (l > 0 && tok[l - 1] == ' ') tok[--l] = '\0';

        if (strcmp(tok, "ctrl") == 0 || strcmp(tok, "control") == 0) {
            out->ctrl = true;
        } else if (strcmp(tok, "alt") == 0) {
            out->alt = true;
        } else if (strcmp(tok, "shift") == 0) {
            out->shift = true;
        } else if (strcmp(tok, "win") == 0 || strcmp(tok, "super") == 0 ||
                   strcmp(tok, "meta") == 0) {
            out->win = true;
        } else {
            WORD vk = key_name_to_vk(tok);
            if (vk == 0 || has_main)   /* 未知键 或 出现第二个主键 */
                return false;
            out->vk = vk;
            has_main = true;
        }
    }
    return out->vk != 0;
}

/* ---------------- 键盘合成 ---------------- */

#define KEY_MODIFIER_COUNT 4                          /* ctrl / alt / shift / win */
#define KEY_INPUT_MAX (KEY_MODIFIER_COUNT * 2 + 2)    /* 修饰键各按下+抬起，加主键按下+抬起 */

/* 带容量的边界检查。不用 static_assert 是因为它在这里做不到实事：KEY_INPUT_MAX
 * 由 KEY_MODIFIER_COUNT 推导而来，任何形如 static_assert(KEY_INPUT_MAX >= 10) 的
 * 断言都恒真——加第五个修饰键时，改了 KEY_MODIFIER_COUNT 它照样通过，忘了改则
 * 数组被写 12 次而它仍然通过。两个方向都拦不住，只会给人虚假的安全感。
 * 真正的兜底是运行期检查容量，把潜在的栈溢出降级为安全丢弃。 */
static void push_key(INPUT *arr, int *n, int cap, WORD vk, bool up)
{
    if (*n >= cap)
        return;
    INPUT e;
    memset(&e, 0, sizeof(e));
    e.type = INPUT_KEYBOARD;
    e.ki.wVk = vk;
    e.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    arr[(*n)++] = e;
}

static bool send_key(const KeyCombo *k)
{
    INPUT in[KEY_INPUT_MAX];
    int n = 0;
    if (k->ctrl)  push_key(in, &n, KEY_INPUT_MAX, VK_CONTROL, false);
    if (k->alt)   push_key(in, &n, KEY_INPUT_MAX, VK_MENU,    false);
    if (k->shift) push_key(in, &n, KEY_INPUT_MAX, VK_SHIFT,   false);
    if (k->win)   push_key(in, &n, KEY_INPUT_MAX, VK_LWIN,    false);
    push_key(in, &n, KEY_INPUT_MAX, k->vk, false);
    push_key(in, &n, KEY_INPUT_MAX, k->vk, true);
    if (k->win)   push_key(in, &n, KEY_INPUT_MAX, VK_LWIN,    true);
    if (k->shift) push_key(in, &n, KEY_INPUT_MAX, VK_SHIFT,   true);
    if (k->alt)   push_key(in, &n, KEY_INPUT_MAX, VK_MENU,    true);
    if (k->ctrl)  push_key(in, &n, KEY_INPUT_MAX, VK_CONTROL, true);

    UINT sent = SendInput((UINT)n, in, sizeof(INPUT));
    if (sent == (UINT)n)
        return true;

    /*
     * 部分注入：序列是「修饰键↓ 主键↓ 主键↑ 修饰键↑」，任何一处中断都会把已按下的
     * 键留在按下状态——修饰键卡住会让此后每次点击都变成 Ctrl+Click，主键卡住则会
     * 触发自动重复、向目标狂灌字符，两者都必须用户手动敲一下才能解除。
     * 主键也要补：中断点若落在「主键↓ 之后、主键↑ 之前」，只放修饰键是不够的。
     * 对本就没按下的键补 KEYUP 是无害的。
     */
    INPUT rel[KEY_MODIFIER_COUNT + 1];
    int m = 0;
    push_key(rel, &m, KEY_MODIFIER_COUNT + 1, k->vk, true);
    if (k->ctrl)  push_key(rel, &m, KEY_MODIFIER_COUNT + 1, VK_CONTROL, true);
    if (k->alt)   push_key(rel, &m, KEY_MODIFIER_COUNT + 1, VK_MENU,    true);
    if (k->shift) push_key(rel, &m, KEY_MODIFIER_COUNT + 1, VK_SHIFT,   true);
    if (k->win)   push_key(rel, &m, KEY_MODIFIER_COUNT + 1, VK_LWIN,    true);
    SendInput((UINT)m, rel, sizeof(INPUT));   /* m 恒 >= 1 */
    HUA_LOG_W("SendInput 仅注入 %u/%d 个事件，已补发按键释放", sent, n);
    return false;
}

/*
 * SendInput 只能送到前台窗口。手势可能开始于非活动窗口，因此键盘类动作
 * 执行前要先激活起点下方锁定的目标。仅临时连接当前前台输入队列，不修改
 * 系统的 ForegroundLockTimeout；后者是 StrokesPlus 旧版采用但过于侵入的做法。
 */
static bool activate_target(HWND target)
{
    if (!target || !IsWindow(target))
        return false;

    HWND foreground = GetForegroundWindow();
    if (foreground == target)
        return true;

    DWORD self_tid = GetCurrentThreadId();
    DWORD foreground_tid = foreground
                               ? GetWindowThreadProcessId(foreground, NULL)
                               : 0;
    bool attached = foreground_tid && foreground_tid != self_tid &&
                    AttachThreadInput(self_tid, foreground_tid, TRUE);

    BOOL activated = SetForegroundWindow(target);

    if (attached)
        AttachThreadInput(self_tid, foreground_tid, FALSE);

    return activated || GetForegroundWindow() == target;
}

/*
 * ShellExecuteW 会把已存在的资源管理器窗口复用掉，但前台锁定策略可能阻止它激活，
 * 结果仅任务栏闪烁。执行前把本次前台权限交给 Shell 处理的进程。
 */
static bool shell_open_foreground(const wchar_t *path)
{
    AllowSetForegroundWindow(ASFW_ANY);
    HINSTANCE result = ShellExecuteW(GetForegroundWindow(), L"open", path,
                                    NULL, NULL, SW_SHOWNORMAL);
    return (INT_PTR)result > 32;   /* ShellExecute 约定：>32 为成功 */
}

static bool send_key_to_target(const KeyCombo *k, HWND target)
{
    if (!activate_target(target))
        return false;
    return send_key(k);
}

/* 单键点按（无修饰），用于音量/媒体键。 */
static bool tap(WORD vk)
{
    KeyCombo k = {0};
    k.vk = vk;
    return send_key(&k);
}

/* ---------------- 内置命令表 ---------------- */

/*
 * 窗口类命令的目标复核。
 *
 * target 是手势**按下时**锁定的窗口，执行要到 END（200ms+ 之后）才发生，其间窗口
 * 可能已自行关闭。Windows 会回收并复用 HWND，故不复核就可能把 WM_CLOSE 发给一个
 * 毫不相干的新窗口。send_key 路径的 activate_target 早就在做这件事了（IsWindow），
 * cmd: 路径此前没有。
 */
static bool target_is_live(HWND target)
{
    return target != NULL && IsWindow(target);
}

/*
 * 最近一次被 hua 最小化的窗口。用途：窗口最小化后桌面就空了，此时在桌面上画
 * 「恢复/最大化」本来是空操作（目标是桌面窗口，ShowWindow 对它没有意义），
 * 改义为把这个窗口拿回来。只记最后一个，不做栈。
 */
static HWND g_last_minimized;

/*
 * 目标是不是桌面本身。桌面图标区（SHELLDLL_DefView / SysListView32）经
 * GA_ROOTOWNER 解析后会落到 Progman 或 WorkerW —— 启用壁纸幻灯片等情况下是后者，
 * 两个都要认，只认 Progman 会在部分机器上失效。
 */
static bool is_desktop_window(HWND w)
{
    if (!w)
        return false;
    if (w == GetShellWindow())
        return true;
    wchar_t cls[32];
    if (!GetClassNameW(w, cls, (int)(sizeof(cls) / sizeof(cls[0]))))
        return false;
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

/*
 * 把最近一次被 hua 最小化的窗口恢复回来。成功返回 true；无记录或记录已失效返回
 * false，调用方按「手势无动作」处理（浮层照常提示，不会静默）。
 */
static bool restore_last_minimized(void)
{
    HWND w = g_last_minimized;
    if (!w)
        return false;

    /* 无论成败都先清空：既避免反复触发，也避免 HWND 被系统回收复用后，
     * 我们拿着一个陈旧句柄去操作一个毫不相干的新窗口。 */
    g_last_minimized = NULL;

    if (!IsWindow(w))
        return false;   /* 窗口已关闭 */
    if (!IsIconic(w))
        return false;   /* 用户已自行还原过，记录过期 */

    ShowWindow(w, SW_RESTORE);
    /* 尽力前置。前台锁定策略下可能被系统拒绝，但 SW_RESTORE 本身通常已带激活，
     * 且「窗口回来了」才是主要目的，故不拿它的返回值判成败。 */
    SetForegroundWindow(w);
    return !IsIconic(w);
}

static bool exec_cmd(const char *name, HWND target)
{
    /*
     * 返回值必须如实反映成败。此前这几条一律 `return target != NULL` —— 「成功」
     * 只等于「指针非空」，而默认 11 条手势里有 5 条走这里。main.c 特意把日志放在
     * 执行**之后**并带上结果，为的就是不让日志断言一件没发生的事；恒真的返回值
     * 把那套设计整个架空了。
     * 注意 ShowWindow 的返回值是「调用前是否可见」，不是成败，故改用状态复核。
     */
    if (strcmp(name, "close_window") == 0) {
        if (!target_is_live(target)) return false;
        return PostMessageW(target, WM_CLOSE, 0, 0) != 0;
    }
    if (strcmp(name, "minimize") == 0) {
        if (!target_is_live(target)) return false;
        ShowWindow(target, SW_MINIMIZE);
        bool ok = IsIconic(target) != 0;
        /* 记住它：窗口收起来后桌面就空了，随后在桌面上画「恢复」即可拿回来。 */
        if (ok)
            g_last_minimized = target;
        return ok;
    }
    /*
     * 下面三条「放大/还原」类命令在**桌面**上都改义为「恢复最近一次被 hua 最小化的
     * 窗口」：目标是桌面窗口时它们本就是空操作，而窗口刚收起、桌面空着的时候，用户
     * 画这几个手势想要的正是把它拿回来。判断必须排在 target_is_live 之前——桌面窗口
     * 本身是"活"的，先过那道检查就会走进对桌面 ShowWindow 的无效分支。
     */
    if (strcmp(name, "maximize") == 0) {
        if (is_desktop_window(target)) return restore_last_minimized();
        if (!target_is_live(target)) return false;
        ShowWindow(target, SW_MAXIMIZE);
        return IsZoomed(target) != 0;
    }
    if (strcmp(name, "restore") == 0) {
        if (is_desktop_window(target)) return restore_last_minimized();
        if (!target_is_live(target)) return false;
        ShowWindow(target, SW_RESTORE);
        /* 还原成功 = 既不最小化也不最大化。 */
        return !IsIconic(target) && !IsZoomed(target);
    }
    if (strcmp(name, "toggle_maximize") == 0) {
        if (is_desktop_window(target)) return restore_last_minimized();
        if (!target_is_live(target)) return false;
        bool was_zoomed = IsZoomed(target) != 0;
        ShowWindow(target, was_zoomed ? SW_RESTORE : SW_MAXIMIZE);
        return (IsZoomed(target) != 0) != was_zoomed;   /* 状态确实翻转了才算成功 */
    }
    if (strcmp(name, "scroll_top") == 0) {
        KeyCombo k = {0}; k.ctrl = true; k.vk = VK_HOME;
        return send_key_to_target(&k, target);
    }
    if (strcmp(name, "scroll_bottom") == 0) {
        KeyCombo k = {0}; k.ctrl = true; k.vk = VK_END;
        return send_key_to_target(&k, target);
    }
    if (strcmp(name, "volume_up") == 0)   return tap(VK_VOLUME_UP);
    if (strcmp(name, "volume_down") == 0) return tap(VK_VOLUME_DOWN);
    if (strcmp(name, "volume_mute") == 0) return tap(VK_VOLUME_MUTE);
    if (strcmp(name, "media_play") == 0)  return tap(VK_MEDIA_PLAY_PAUSE);
    if (strcmp(name, "copy") == 0) {
        KeyCombo k = {0}; k.ctrl = true; k.vk = 'C';
        return send_key_to_target(&k, target);
    }
    if (strcmp(name, "paste") == 0) {
        KeyCombo k = {0}; k.ctrl = true; k.vk = 'V';
        return send_key_to_target(&k, target);
    }
    if (strcmp(name, "open_exe_dir") == 0) {
        /* 打开目标窗口所属进程 exe 所在目录（资源管理器）。 */
        if (!target)
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        if (!pid)
            return false;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h)
            return false;
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(h, 0, path, &n);
        CloseHandle(h);
        if (!ok)
            return false;
        wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash)
            return false;
        *slash = L'\0';   /* 截断文件名，保留目录 */
        return shell_open_foreground(path);
    }
    return false;   /* 未知命令 */
}

/* ---------------- 友好显示名 ---------------- */

const char *action_label(const char *value)
{
    if (!value)
        return "";

    if (strncmp(value, "cmd:", 4) == 0) {
        const char *n = value + 4;
        if (!strcmp(n, "close_window"))    return "关闭窗口";
        if (!strcmp(n, "minimize"))        return "最小化";
        if (!strcmp(n, "maximize"))        return "最大化";
        if (!strcmp(n, "restore"))         return "还原";
        if (!strcmp(n, "toggle_maximize")) return "最大化/还原";
        if (!strcmp(n, "scroll_top"))      return "滚到顶部";
        if (!strcmp(n, "scroll_bottom"))   return "滚到底部";
        if (!strcmp(n, "volume_up"))       return "音量 +";
        if (!strcmp(n, "volume_down"))     return "音量 -";
        if (!strcmp(n, "volume_mute"))     return "静音";
        if (!strcmp(n, "media_play"))      return "播放/暂停";
        if (!strcmp(n, "copy"))            return "复制";
        if (!strcmp(n, "paste"))           return "粘贴";
        if (!strcmp(n, "open_exe_dir"))    return "打开程序目录";
        return n;
    }
    if (strncmp(value, "key:", 4) == 0) {
        const char *k = value + 4;
        if (!_stricmp(k, "alt+left"))  return "后退";
        if (!_stricmp(k, "alt+right")) return "前进";
        if (!_stricmp(k, "f5"))        return "刷新";
        if (!_stricmp(k, "ctrl+w"))    return "关闭标签页";
        if (!_stricmp(k, "ctrl+t"))    return "新建标签页";
        if (!_stricmp(k, "delete"))    return "删除";
        if (!_stricmp(k, "esc"))       return "Esc";
        return k;   /* 其余显示按键组合本身，如 ctrl+shift+t */
    }
    if (strncmp(value, "run:", 4) == 0)
        return "运行程序";
    return value;
}

/* ---------------- 分发 ---------------- */

bool action_execute(const char *value, HWND target)
{
    if (!value)
        return false;

    if (strncmp(value, "key:", 4) == 0) {
        KeyCombo k;
        if (!action_parse_key(value + 4, &k))
            return false;
        return send_key_to_target(&k, target);
    }
    if (strncmp(value, "run:", 4) == 0) {
        wchar_t *w = hua_utf8_to_utf16(value + 4);
        if (!w)
            return false;
        HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        hua_free(w);
        return (INT_PTR)r > 32;   /* ShellExecute 约定：>32 为成功 */
    }
    if (strncmp(value, "cmd:", 4) == 0) {
        return exec_cmd(value + 4, target);
    }
    return false;
}
