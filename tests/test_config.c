/*
 * test_config.c —— ini 解析与数据模型单元测试（utest.h）。
 */
#include "config.h"
#include "utest.h"

UTEST(config, defaults)
{
    Config c;
    config_set_defaults(&c);
    ASSERT_EQ(c.trigger, CFG_TRIGGER_RIGHT);
    ASSERT_EQ(c.min_distance, 20);
    ASSERT_EQ(c.step_distance, 12);
    ASSERT_EQ(c.tolerance, 0);                       /* 我们的默认：精确 */
    ASSERT_EQ(c.filter_mode, CFG_FILTER_BLACKLIST);
    ASSERT_TRUE(c.disable_on_fullscreen);
    ASSERT_EQ((int)c.gesture_count, 0);
    ASSERT_EQ((int)c.app_count, 0);

    /* MouseInc 对齐的可调项默认值 */
    ASSERT_EQ(c.trigger_distance, 5);
    ASSERT_TRUE(c.trail_arrow);
    ASSERT_FALSE(c.random_color);
    ASSERT_EQ((int)c.fail_color, 0x666666);
    ASSERT_EQ(c.text_size, 26);
    ASSERT_EQ(c.text_position, 150);
    ASSERT_EQ(c.pause_timeout, 1000);
    ASSERT_TRUE(c.restore_event);
}

UTEST(config, parse_mouseinc_items)
{
    Config c;
    const char *ini =
        "[General]\n"
        "TriggerDistance = 8\n"
        "TrailArrow = false\n"
        "RandomColor = true\n"
        "FailColor = FF0000\n"
        "TextSize = 20\n"
        "TextPosition = 100\n"
        "PauseTimeout = 1500\n"
        "RestoreEvent = false\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ(c.trigger_distance, 8);
    ASSERT_FALSE(c.trail_arrow);
    ASSERT_TRUE(c.random_color);
    ASSERT_EQ((int)c.fail_color, 0xFF0000);
    ASSERT_EQ(c.text_size, 20);
    ASSERT_EQ(c.text_position, 100);
    ASSERT_EQ(c.pause_timeout, 1500);
    ASSERT_FALSE(c.restore_event);
}

UTEST(config, parse_general_fields)
{
    Config c;
    const char *ini =
        "[General]\n"
        "Trigger = middle\n"
        "MinDistance = 30\n"
        "StepDistance = 8\n"
        "Tolerance = 1\n"
        "FilterMode = whitelist\n"
        "DisableOnFullscreen = false\n"
        "TrailWidth = 5\n"
        "TrailColor = FF8800\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ(c.trigger, CFG_TRIGGER_MIDDLE);
    ASSERT_EQ(c.min_distance, 30);
    ASSERT_EQ(c.step_distance, 8);
    ASSERT_EQ(c.tolerance, 1);
    ASSERT_EQ(c.filter_mode, CFG_FILTER_WHITELIST);
    ASSERT_FALSE(c.disable_on_fullscreen);
    ASSERT_EQ(c.trail_width, 5);
    ASSERT_EQ((int)c.trail_color, 0xFF8800);
}

UTEST(config, parse_global_gestures)
{
    Config c;
    const char *ini =
        "[Gestures]\n"
        "6 = key:alt+right\n"
        "26 = cmd:close_window\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ((int)c.gesture_count, 2);
    ASSERT_STREQ(config_lookup_global(&c, "6"), "key:alt+right");
    ASSERT_STREQ(config_lookup_global(&c, "26"), "cmd:close_window");
    ASSERT_TRUE(config_lookup_global(&c, "9") == NULL);
}

UTEST(config, inline_comment_stripped)
{
    Config c;
    const char *ini =
        "[Gestures]\n"
        "6 = key:alt+right   ; 前进\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_STREQ(config_lookup_global(&c, "6"), "key:alt+right");
}

UTEST(config, app_override_and_enabled)
{
    Config c;
    const char *ini =
        "[Gestures]\n"
        "26 = cmd:close_window\n"
        "[App:chrome.exe]\n"
        "26 = key:ctrl+w\n"
        "[App:game.exe]\n"
        "Enabled = false\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ((int)c.app_count, 2);

    const AppConfig *chrome = config_find_app(&c, "chrome.exe");
    ASSERT_TRUE(chrome != NULL);
    ASSERT_TRUE(chrome->enabled);
    ASSERT_STREQ(config_lookup_app(chrome, "26"), "key:ctrl+w");

    const AppConfig *game = config_find_app(&c, "game.exe");
    ASSERT_TRUE(game != NULL);
    ASSERT_FALSE(game->enabled);
}

UTEST(config, app_name_case_insensitive)
{
    Config c;
    const char *ini =
        "[App:Chrome.EXE]\n"
        "26 = key:ctrl+w\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    /* 存储与查找都按小写。 */
    ASSERT_TRUE(config_find_app(&c, "chrome.exe") != NULL);
}

UTEST(config, bad_line_skipped)
{
    Config c;
    const char *ini =
        "[Gestures]\n"
        "this line has no equals sign\n"
        "6 = key:alt+right\n";
    /* 坏行跳过，好行仍生效。 */
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_STREQ(config_lookup_global(&c, "6"), "key:alt+right");
}

UTEST(config, bom_is_stripped)
{
    Config c;
    const char *ini =
        "\xEF\xBB\xBF[General]\n"
        "MinDistance = 42\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ(c.min_distance, 42);
}

UTEST(config, missing_optional_keeps_default)
{
    Config c;
    const char *ini =
        "[General]\n"
        "MinDistance = 99\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ(c.min_distance, 99);
    ASSERT_EQ(c.step_distance, 12);   /* 未给，保持默认 */
    ASSERT_EQ(c.trigger, CFG_TRIGGER_RIGHT);
}

/* ---------------- 门控 config_app_enabled ---------------- */

UTEST(gate, blacklist_normal_app_enabled)
{
    Config c;
    config_parse_string(&c, "[General]\nFilterMode=blacklist\n");
    ASSERT_TRUE(config_app_enabled(&c, "notepad.exe", false));
}

UTEST(gate, disabled_app_blocked)
{
    Config c;
    config_parse_string(&c, "[App:game.exe]\nEnabled=false\n");
    ASSERT_FALSE(config_app_enabled(&c, "game.exe", false));
    ASSERT_TRUE(config_app_enabled(&c, "other.exe", false));
}

UTEST(gate, fullscreen_blocked_when_configured)
{
    Config c;
    config_parse_string(&c, "[General]\nDisableOnFullscreen=true\n");
    ASSERT_FALSE(config_app_enabled(&c, "game.exe", true));
    ASSERT_TRUE(config_app_enabled(&c, "game.exe", false));
}

UTEST(gate, fullscreen_allowed_when_disabled_option_off)
{
    Config c;
    config_parse_string(&c, "[General]\nDisableOnFullscreen=false\n");
    ASSERT_TRUE(config_app_enabled(&c, "game.exe", true));
}

UTEST(gate, whitelist_blocks_unlisted)
{
    Config c;
    config_parse_string(&c,
        "[General]\nFilterMode=whitelist\n"
        "[App:chrome.exe]\n6=key:alt+right\n");
    ASSERT_TRUE(config_app_enabled(&c, "chrome.exe", false));
    ASSERT_FALSE(config_app_enabled(&c, "notepad.exe", false));
}

/* ---------------- 解析 config_resolve ---------------- */

UTEST(resolve, global_only)
{
    Config c;
    config_parse_string(&c, "[Gestures]\n6=key:alt+right\n");
    ASSERT_STREQ(config_resolve(&c, "notepad.exe", "6"), "key:alt+right");
    ASSERT_STREQ(config_resolve(&c, NULL, "6"), "key:alt+right");
    ASSERT_TRUE(config_resolve(&c, "notepad.exe", "8") == NULL);
}

UTEST(resolve, app_overrides_global)
{
    Config c;
    config_parse_string(&c,
        "[Gestures]\n26=cmd:close_window\n"
        "[App:chrome.exe]\n26=key:ctrl+w\n");
    /* chrome 里 26 → 关标签页；其他程序 26 → 关窗口 */
    ASSERT_STREQ(config_resolve(&c, "chrome.exe", "26"), "key:ctrl+w");
    ASSERT_STREQ(config_resolve(&c, "notepad.exe", "26"), "cmd:close_window");
}

UTEST(resolve, app_falls_back_to_global)
{
    Config c;
    config_parse_string(&c,
        "[Gestures]\n6=key:alt+right\n"
        "[App:chrome.exe]\n26=key:ctrl+w\n");
    /* chrome 没定义 6，回退全局 */
    ASSERT_STREQ(config_resolve(&c, "chrome.exe", "6"), "key:alt+right");
}

UTEST(resolve, app_none_suppresses_global)
{
    Config c;
    config_parse_string(&c,
        "[Gestures]\n39=key:f5\n"
        "[App:powerpnt.exe]\n39=cmd:none\n");
    /* PPT 里 39 被显式屏蔽：必须返回 NULL，绝不能回落到全局的 key:f5 */
    ASSERT_TRUE(config_resolve(&c, "powerpnt.exe", "39") == NULL);
    /* 其他程序不受影响 */
    ASSERT_STREQ(config_resolve(&c, "notepad.exe", "39"), "key:f5");
}

UTEST(resolve, global_none_is_no_action)
{
    Config c;
    config_parse_string(&c, "[Gestures]\n39=cmd:none\n");
    ASSERT_TRUE(config_resolve(&c, "notepad.exe", "39") == NULL);
}

UTEST(resolve, none_is_case_insensitive)
{
    Config c;
    config_parse_string(&c,
        "[Gestures]\n39=key:f5\n"
        "[App:foo.exe]\n39=CMD:NONE\n");
    ASSERT_TRUE(config_resolve(&c, "foo.exe", "39") == NULL);
}

UTEST(resolve, tolerance_fuzzy)
{
    Config c;
    config_parse_string(&c, "[General]\nTolerance=1\n[Gestures]\n26=cmd:close_window\n");
    /* 画成 "266"，编辑距离 1，容错命中 */
    ASSERT_STREQ(config_resolve(&c, "notepad.exe", "266"), "cmd:close_window");
}

UTEST_MAIN();
