/*
 * watchdog.c —— 见 watchdog.h。
 */
#include "watchdog.h"

bool watchdog_should_reinstall(WatchdogState *st, const WatchdogSample *s)
{
    /* 没装上也要报告为「需要装」。hook_install 失败会让句柄停在空，
     * 若此处返回 false，探活就在最需要它的场景（安装失败）里永久失能。 */
    if (!s->installed)
        return true;

    if (!s->cursor_ok)
        return false;   /* 零信息：保留计数 */

    /* prev_tick == 0 是首次采样，还没有可比的基线，任何结论都无依据。 */
    bool alive   = (s->prev_tick != 0 && s->last_cb_tick >= s->prev_tick);
    bool suspect = (s->prev_tick != 0 && s->moved && s->last_cb_tick < s->prev_tick);

    if (alive) {
        st->suspect = 0;
        return false;
    }
    if (!suspect)
        return false;   /* 零信息：保留计数，绝不清零 */

    st->suspect++;
    if (st->suspect < WATCHDOG_SUSPECT_THRESHOLD)
        return false;
    st->suspect = 0;
    return true;
}
