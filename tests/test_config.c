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

UTEST(config, clamps_out_of_range_values)
{
    Config c;
    /* ini 是不可信输入：超大值会导致平方后有符号溢出（long 在 Windows 是 32 位），
     * 且 Tolerance 过大会让任何甩动都匹配上模板并真的执行动作。 */
    config_parse_string(&c,
        "[General]\n"
        "Tolerance=999\n"
        "MinDistance=2000000000\n"
        "StepDistance=2000000000\n"
        "TriggerDistance=2000000000\n"
        "TrailWidth=2000000000\n"
        "TextSize=2000000000\n"
        "TextOutlineWidth=999999\n"
        "TextLetterSpacing=999999\n");
    ASSERT_EQ(c.tolerance, CFG_MAX_TOLERANCE);
    ASSERT_EQ(c.min_distance, 10000);
    ASSERT_EQ(c.step_distance, 10000);
    ASSERT_EQ(c.trigger_distance, 10000);
    ASSERT_EQ(c.trail_width, 200);
    ASSERT_EQ(c.text_size, 500);
    ASSERT_EQ(c.text_outline_width, 100);
    ASSERT_EQ(c.text_letter_spacing, 500);
}

UTEST(config, invalid_positive_fields_fall_back_to_defaults)
{
    Config c;
    /* 非正值（含 atoi 对垃圾输入返回的 0）必须回落到**文档默认值**，而不是区间下界。
     * 若夹成 1，MinDistance=1 会让每次像素抖动都成为一个方向段 → 所有手势失效。 */
    config_parse_string(&c,
        "[General]\n"
        "MinDistance=0\n"
        "StepDistance=-5\n"
        "TriggerDistance=-1\n"
        "TrailWidth=0\n"
        "TextSize=-100\n");
    ASSERT_EQ(c.min_distance, 20);
    ASSERT_EQ(c.step_distance, 12);
    ASSERT_EQ(c.trigger_distance, 5);
    ASSERT_EQ(c.trail_width, 3);
    ASSERT_EQ(c.text_size, 26);
}

UTEST(config, garbage_text_falls_back_to_defaults)
{
    Config c;
    /* atoi 对非数字返回 0。中文用户误填中文数字并不罕见。 */
    config_parse_string(&c,
        "[General]\n"
        "MinDistance=二十\n"
        "StepDistance=auto\n"
        "TextSize=O\n"
        "TrailWidth=\n");
    ASSERT_EQ(c.min_distance, 20);
    ASSERT_EQ(c.step_distance, 12);
    ASSERT_EQ(c.text_size, 26);
    ASSERT_EQ(c.trail_width, 3);
}

UTEST(config, zero_is_meaningful_for_some_fields)
{
    Config c;
    /* 这些字段的 0 是合法语义，不能被回落掉。 */
    config_parse_string(&c,
        "[General]\n"
        "Tolerance=0\n"
        "TrailMaxLength=0\n"
        "TextOutlineWidth=0\n"
        "TextLetterSpacing=0\n");
    ASSERT_EQ(c.tolerance, 0);
    ASSERT_EQ(c.trail_max_length, 0);
    ASSERT_EQ(c.text_outline_width, 0);
    ASSERT_EQ(c.text_letter_spacing, 0);
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
    /* 画成 "266"，编辑距离 1 < 模板长度 2，容错命中 */
    ASSERT_STREQ(config_resolve(&c, "notepad.exe", "266"), "cmd:close_window");
}

UTEST(resolve, tolerance_is_clamped_to_sane_upper_bound)
{
    Config c;
    /* Tolerance 仍是用户主动开启的模糊匹配（默认 0 = 精确），这里只保证
     * 荒谬的值被夹到有意义的上界，不改变容错本身的语义。 */
    config_parse_string(&c, "[General]\nTolerance=99\n[Gestures]\n26=cmd:close_window\n");
    ASSERT_EQ(c.tolerance, CFG_MAX_TOLERANCE);
}

UTEST_MAIN();
