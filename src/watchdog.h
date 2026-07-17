/*
 * watchdog.h —— 钩子探活的判定逻辑。纯逻辑、无 Win32 依赖，便于单测。
 *
 * 为什么要单独抽出来：系统在 LL 钩子回调超时后会静默摘钩且不通知，而 Win32 没有
 * 「查询钩子是否还在」的 API，只能靠启发式推断。这个推断写错过两次（一次把「没装上」
 * 当成健康、一次把「光标没动」当成存活证据），两次都是在零测试的 Win32 代码里靠肉眼
 * review 才发现的。把判定做成纯函数后，这些场景可以直接用表驱动测试钉死。
 *
 * 采样与副作用（GetCursorPos / GetTickCount64 / 重装钩子）留在 hook.c，这里只做判断。
 */
#ifndef HUA_WATCHDOG_H
#define HUA_WATCHDOG_H

#include <stdbool.h>

/* 连续多少个周期「可疑」才判定失效。
 * >1 是为了滤误判：安全桌面（UAC 同意框、锁屏、Ctrl+Alt+Del）期间鼠标事件不派发到
 * 我们的钩子，但光标位置是全局的、照样在变；SetCursorPos 类工具（mouse jiggler）
 * 移动光标同样不产生鼠标事件。单周期就下结论会误重装。 */
#define WATCHDOG_SUSPECT_THRESHOLD 2

typedef struct {
    int suspect;   /* 连续可疑周期数 */
} WatchdogState;

/* 一次探活采样。全部是纯数值，由调用方从 Win32 取得。 */
typedef struct {
    bool               installed;      /* 钩子句柄非空（安装失败时为 false） */
    bool               cursor_ok;      /* GetCursorPos 是否成功 */
    bool               moved;          /* 光标相对上次采样是否移动过 */
    unsigned long long last_cb_tick;   /* 最近一次钩子回调的时刻 */
    unsigned long long prev_tick;      /* 上次采样时刻；0 = 尚无基线（首次） */
} WatchdogSample;

/*
 * 喂入一次采样，返回是否应当（重）安装钩子。
 *
 * 三态，不是两态 —— 这是关键：
 *   - 确证存活：本周期内收到过回调。**唯一**能清零计数的证据。
 *   - 可疑    ：光标动过，却一个回调都没收到。
 *   - 零信息  ：光标没动 / 取不到光标。既不能证明活也不能证明死，
 *               必须保留已积累的计数——若当作健康证据清零，钩子死后用户以最常见的
 *               间歇方式用鼠标（动→停→动→停），计数永远到不了阈值，探活形同虚设。
 */
bool watchdog_should_reinstall(WatchdogState *st, const WatchdogSample *s);

#endif /* HUA_WATCHDOG_H */
