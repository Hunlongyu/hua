/*
 * 钩子探活判定的单测。
 *
 * 这些用例存在的理由很具体：该判定曾两次写错，两次都逃过了肉眼 review，
 * 直到实机才发现。每条用例对应一个真实踩过的坑，注释里写明是哪个。
 */
#include "watchdog.h"
#include "utest.h"

/* 便捷构造：默认「已安装、光标可读」。 */
static WatchdogSample mk(bool moved, unsigned long long last_cb, unsigned long long prev)
{
    WatchdogSample s;
    s.installed = true;
    s.cursor_ok = true;
    s.moved = moved;
    s.last_cb_tick = last_cb;
    s.prev_tick = prev;
    return s;
}

UTEST(watchdog, not_installed_must_reinstall)
{
    WatchdogState st = {0};
    WatchdogSample s = mk(false, 0, 0);
    s.installed = false;
    /* 曾经写成 return false —— 于是 hook_install 失败后句柄停在空，
     * 探活每次都在第一行返回「健康」，永不重试，手势整个会话失效。 */
    ASSERT_TRUE(watchdog_should_reinstall(&st, &s));
}

UTEST(watchdog, first_sample_never_decides)
{
    WatchdogState st = {0};
    /* prev_tick == 0：还没有基线，即便 moved 也不能下结论。 */
    WatchdogSample s = mk(true, 0, 0);
    ASSERT_FALSE(watchdog_should_reinstall(&st, &s));
    ASSERT_EQ(st.suspect, 0);
}

UTEST(watchdog, callback_in_window_proves_alive)
{
    WatchdogState st = {0};
    /* last_cb_tick >= prev_tick：本周期收到过回调 → 确证存活。 */
    WatchdogSample s = mk(true, 1000, 1000);
    ASSERT_FALSE(watchdog_should_reinstall(&st, &s));
    ASSERT_EQ(st.suspect, 0);
}

UTEST(watchdog, alive_clears_accumulated_suspicion)
{
    WatchdogState st = {0};
    WatchdogSample suspicious = mk(true, 500, 1000);   /* 动过，回调早于基线 */
    ASSERT_FALSE(watchdog_should_reinstall(&st, &suspicious));
    ASSERT_EQ(st.suspect, 1);

    WatchdogSample alive = mk(true, 2000, 1500);       /* 收到回调 */
    ASSERT_FALSE(watchdog_should_reinstall(&st, &alive));
    ASSERT_EQ(st.suspect, 0);                          /* 计数被清掉 */
}

UTEST(watchdog, one_suspect_cycle_is_not_enough)
{
    WatchdogState st = {0};
    /* 单周期不下结论：安全桌面/UAC 期间光标会动但事件不进我们的钩子，
     * 误重装若撞上进行中的手势会清掉 suppress 标记 → 孤儿 Up 泄漏。 */
    WatchdogSample s = mk(true, 500, 1000);
    ASSERT_FALSE(watchdog_should_reinstall(&st, &s));
    ASSERT_EQ(st.suspect, 1);
}

UTEST(watchdog, two_consecutive_suspect_cycles_trigger)
{
    WatchdogState st = {0};
    WatchdogSample s1 = mk(true, 500, 1000);
    ASSERT_FALSE(watchdog_should_reinstall(&st, &s1));
    WatchdogSample s2 = mk(true, 500, 1500);
    ASSERT_TRUE(watchdog_should_reinstall(&st, &s2));
    ASSERT_EQ(st.suspect, 0);   /* 触发后复位，避免连环重装 */
}

UTEST(watchdog, intermittent_use_still_detects_dead_hook)
{
    WatchdogState st = {0};
    /*
     * **这条是核心回归用例。**
     * 曾经写成 suspect = moved && ...，当光标没动时把计数清零 —— 但「光标没动」
     * 对钩子死活是零信息，不是健康证据。于是钩子死后，用户以最常见的间歇方式
     * 用鼠标（动→停→动→停），计数永远到不了阈值，探活无限期不触发——恰恰在它
     * 唯一存在的理由上失效。
     */
    unsigned long long t = 1000;
    WatchdogSample moved_no_cb = mk(true, 500, t);      /* 动了，无回调 */
    ASSERT_FALSE(watchdog_should_reinstall(&st, &moved_no_cb));
    ASSERT_EQ(st.suspect, 1);

    t += 3000;
    WatchdogSample idle = mk(false, 500, t);            /* 停下来看屏幕：零信息 */
    ASSERT_FALSE(watchdog_should_reinstall(&st, &idle));
    ASSERT_EQ(st.suspect, 1);                          /* 证据必须保住，不能清零 */

    t += 3000;
    WatchdogSample moved_again = mk(true, 500, t);      /* 又动了，仍无回调 */
    ASSERT_TRUE(watchdog_should_reinstall(&st, &moved_again));
}

UTEST(watchdog, cursor_unavailable_is_no_information)
{
    WatchdogState st = {0};
    WatchdogSample s1 = mk(true, 500, 1000);
    ASSERT_FALSE(watchdog_should_reinstall(&st, &s1));
    ASSERT_EQ(st.suspect, 1);

    /* 取不到光标（切到非输入桌面时会失败）同样是零信息，
     * 不能把已积累的证据丢掉。 */
    WatchdogSample blind = mk(true, 500, 1500);
    blind.cursor_ok = false;
    ASSERT_FALSE(watchdog_should_reinstall(&st, &blind));
    ASSERT_EQ(st.suspect, 1);

    /* 恢复后再来一个可疑周期即应触发。 */
    WatchdogSample s2 = mk(true, 500, 2000);
    ASSERT_TRUE(watchdog_should_reinstall(&st, &s2));
}

UTEST(watchdog, idle_alone_never_triggers)
{
    WatchdogState st = {0};
    /* 纯键盘用户 / 长时间不碰鼠标：全程零信息，不应误判失效。 */
    for (int i = 0; i < 100; i++) {
        WatchdogSample s = mk(false, 500, 1000 + (unsigned long long)i * 3000);
        ASSERT_FALSE(watchdog_should_reinstall(&st, &s));
    }
    ASSERT_EQ(st.suspect, 0);
}

UTEST_MAIN();
