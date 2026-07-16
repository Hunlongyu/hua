/*
 * hook.h —— 全局低级鼠标钩子（WH_MOUSE_LL）+ 触发状态机 + 采点 + 右键吞补发。
 *
 * 生命周期（一次手势）：
 *   触发键 Down → 锁定目标窗口、吞掉 Down、进入 Tentative、起点入缓冲
 *   Move        → 按 StepDistance 去抖采点；累计位移过 MinDistance → Active
 *   触发键 Up   → Active: 编码方向串、PostMessage(GESTURE_END)、吞掉 Up
 *                 Tentative(仅点击): 补发原生按键（下+上）还原、吞掉 Up
 *
 * 关键纪律：回调里只做判定/采点/编码（纯 CPU，微秒级），绝不执行动作
 * （SendInput/ShellExecute 可能超过 LL 钩子 300ms 超时被系统摘钩）——
 * 动作在主线程收到 GESTURE_END 后执行。
 */
#ifndef HUA_HOOK_H
#define HUA_HOOK_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include "recognizer.h"   /* Pt */

typedef enum {
    HUA_TRIGGER_RIGHT = 0,
    HUA_TRIGGER_MIDDLE,
    HUA_TRIGGER_X1,
    HUA_TRIGGER_X2,
} HuaTrigger;

/*
 * 安装钩子。notify_hwnd 收 WM_HUA_HOOK 上报。
 * trigger_dist：按下后移动多远才进入 Active（开始手势）。
 * min_dist：方向分段阈值（识别灵敏度，传给 recognizer）。
 * step_dist：采点最小间隔（去抖）。成功返回 true。
 */
bool hook_install(HWND notify_hwnd, HuaTrigger trigger,
                  int trigger_dist, int min_dist, int step_dist);
void hook_uninstall(void);

/* 钩子探活（启发式：光标动过但钩子无事件 → 已被系统静默摘掉）。
 * 需由主线程定期调用；返回 true 表示应当（重）安装钩子。
 * 未安装（含安装失败）时也返回 true。连续两周期可疑才判定，以滤除安全桌面误判。 */
bool hook_looks_dead(void);

/* 状态机是否空闲（无手势进行中）。用于避免在手势中途重装钩子。 */
bool hook_is_idle(void);

/* 供主线程在收到 GESTURE_END 后读取：最近一次识别的方向串与锁定的目标窗口。 */
const char *hook_last_seq(void);
HWND        hook_last_target(void);

/* 主线程逐帧轮询真实光标位置，弥补部分环境中 WH_MOUSE_LL 丢失 Move 的情况。 */
void hook_poll_cursor(void);

/* 供 overlay 实时取当前轨迹点（与钩子同线程，安全）。返回点数，*out 指向内部缓冲。 */
size_t hook_snapshot(const Pt **out);

/* 手势停顿超时瞬间的诊断现场，供主线程写详细日志。 */
typedef struct {
    ULONGLONG         idle_ms;
    DWORD             timeout_ms;
    size_t            point_count;
    Pt                start;
    Pt                last;
    Pt                cursor;
    bool              cursor_valid;
    HWND              target;
    HWND              foreground_at_down;
    HWND              foreground_at_timeout;
} HookTimeoutInfo;

/*
 * 主线程重绘时调用。若 Active 手势连续 timeout_ms 没有光标位移，则按
 * PauseTimeout 策略取消手势并返回 true；info 非 NULL 时写入超时现场。
 */
bool hook_cancel_if_timed_out(DWORD timeout_ms, HookTimeoutInfo *info);

/* 补发一次原生触发键点击（下+上）。仅用于未形成轨迹时还原原生行为。 */
void hook_replay_trigger_click(void);

/*
 * 门控回调：在触发键按下时调用，target 是按下点下方锁定的顶层窗口；
 * 返回 false 则本次不进入手势（放行原生按键）。传 NULL 清除。
 */
typedef bool (*HookGateFn)(HWND target);
void hook_set_gate(HookGateFn fn);

#endif /* HUA_HOOK_H */
