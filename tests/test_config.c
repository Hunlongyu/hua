/*
 * test_config.c —— ini 解析与数据模型单元测试（utest.h）。
 */
#include "config.h"
#include "utest.h"

#include <stdio.h>

UTEST(config, defaults)
{
    Config c;
    config_set_defaults(&c);
    ASSERT_EQ(c.trigger, CFG_TRIGGER_RIGHT);
    ASSERT_EQ(c.min_distance, 20);
    ASSERT_EQ(c.step_distance, 12);
    ASSERT_EQ(c.match_score, CFG_DEFAULT_MATCH_SCORE);
    ASSERT_EQ(c.ambiguity_margin, CFG_DEFAULT_AMBIGUITY_MARGIN);
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
    ASSERT_TRUE(c.log_enabled);
    ASSERT_EQ(c.log_level, CFG_LOG_WARN);
    ASSERT_EQ(c.log_max_size_mb, 10);
    ASSERT_EQ(c.log_retention_days, 2);
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
     * 而越界识别阈值会让拒识策略失去意义。 */
    config_parse_string(&c,
        "[General]\n"
        "MatchScore=999\n"
        "AmbiguityMargin=999\n"
        "MinDistance=2000000000\n"
        "StepDistance=2000000000\n"
        "TriggerDistance=2000000000\n"
        "TrailWidth=2000000000\n"
        "TextSize=2000000000\n"
        "TextOutlineWidth=999999\n"
        "TextLetterSpacing=999999\n"
        "LogMaxSizeMB=999999\n"
        "LogRetentionDays=999999\n");
    ASSERT_EQ(c.match_score, 100);
    ASSERT_EQ(c.ambiguity_margin, 30);
    ASSERT_EQ(c.min_distance, 10000);
    ASSERT_EQ(c.step_distance, 10000);
    ASSERT_EQ(c.trigger_distance, 10000);
    ASSERT_EQ(c.trail_width, 200);
    ASSERT_EQ(c.text_size, 500);
    ASSERT_EQ(c.text_outline_width, 100);
    ASSERT_EQ(c.text_letter_spacing, 500);
    ASSERT_EQ(c.log_max_size_mb, 1024);
    ASSERT_EQ(c.log_retention_days, 3650);
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
        "MatchScore=0\n"
        "TrailWidth=0\n"
        "TextSize=-100\n"
        "LogMaxSizeMB=0\n"
        "LogRetentionDays=-1\n");
    ASSERT_EQ(c.min_distance, 20);
    ASSERT_EQ(c.step_distance, 12);
    ASSERT_EQ(c.trigger_distance, 5);
    ASSERT_EQ(c.match_score, CFG_DEFAULT_MATCH_SCORE);
    ASSERT_EQ(c.trail_width, 3);
    ASSERT_EQ(c.text_size, 26);
    ASSERT_EQ(c.log_max_size_mb, 10);
    ASSERT_EQ(c.log_retention_days, 2);
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
        "TrailWidth=\n"
        "LogMaxSizeMB=ten\n"
        "LogRetentionDays=seven\n");
    ASSERT_EQ(c.min_distance, 20);
    ASSERT_EQ(c.step_distance, 12);
    ASSERT_EQ(c.text_size, 26);
    ASSERT_EQ(c.trail_width, 3);
    ASSERT_EQ(c.log_max_size_mb, 10);
    ASSERT_EQ(c.log_retention_days, 2);
}

UTEST(config, zero_is_meaningful_for_some_fields)
{
    Config c;
    /* 这些字段的 0 是合法语义，不能被回落掉。 */
    config_parse_string(&c,
        "[General]\n"
        "AmbiguityMargin=0\n"
        "TrailMaxLength=0\n"
        "TextOutlineWidth=0\n"
        "TextLetterSpacing=0\n");
    ASSERT_EQ(c.ambiguity_margin, 0);
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
        "MatchScore = 78\n"
        "AmbiguityMargin = 9\n"
        "FilterMode = whitelist\n"
        "DisableOnFullscreen = false\n"
        "LogEnabled = false\n"
        "LogLevel = error\n"
        "LogMaxSizeMB = 25\n"
        "LogRetentionDays = 30\n"
        "TrailWidth = 5\n"
        "TrailColor = FF8800\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ(c.trigger, CFG_TRIGGER_MIDDLE);
    ASSERT_EQ(c.min_distance, 30);
    ASSERT_EQ(c.step_distance, 8);
    ASSERT_EQ(c.match_score, 78);
    ASSERT_EQ(c.ambiguity_margin, 9);
    ASSERT_EQ(c.filter_mode, CFG_FILTER_WHITELIST);
    ASSERT_FALSE(c.disable_on_fullscreen);
    ASSERT_FALSE(c.log_enabled);
    ASSERT_EQ(c.log_level, CFG_LOG_ERROR);
    ASSERT_EQ(c.log_max_size_mb, 25);
    ASSERT_EQ(c.log_retention_days, 30);
    ASSERT_EQ(c.trail_width, 5);
    ASSERT_EQ((int)c.trail_color, 0xFF8800);
}

UTEST(config, log_levels_and_invalid_fallback)
{
    Config c;
    config_parse_string(&c, "[General]\nLogLevel=warning\n");
    ASSERT_EQ(c.log_level, CFG_LOG_WARN);
    ASSERT_EQ(c.diag.bad_values, 0);

    config_parse_string(&c, "[General]\nLogLevel=verbose\n");
    ASSERT_EQ(c.log_level, CFG_LOG_WARN);
    ASSERT_EQ(c.diag.bad_values, 1);
    ASSERT_STREQ(c.diag.first_issue, "LogLevel");
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

UTEST(config, gesture_keys_are_normalized_and_last_duplicate_wins)
{
    Config c;
    ASSERT_TRUE(config_parse_string(&c,
        "[Gestures]\n6=key:alt+right\n66=key:ctrl+right\n"));
    ASSERT_EQ((int)c.gesture_count, 1);
    ASSERT_STREQ(c.gestures[0].key, "6");
    ASSERT_STREQ(config_lookup_global(&c, "6"), "key:ctrl+right");
    ASSERT_TRUE(config_lookup_global(&c, "66") == NULL); /* 查找接口仍是严格标准 key */
    ASSERT_EQ(c.diag.duplicate_gestures, 1);
    ASSERT_STREQ(c.diag.first_issue, "66");
}

UTEST(config, invalid_gesture_keys_are_reported_and_dropped)
{
    Config c;
    ASSERT_TRUE(config_parse_string(&c,
        "[Gestures]\n5=cmd:minimize\nhello=cmd:close_window\n6=key:alt+right\n"));
    ASSERT_EQ((int)c.gesture_count, 1);
    ASSERT_STREQ(c.gestures[0].key, "6");
    ASSERT_EQ(c.diag.invalid_gestures, 2);
    ASSERT_STREQ(c.diag.first_issue, "5");
}

UTEST(config, gesture_key_length_boundary_is_enforced_after_normalization)
{
    Config c;
    ASSERT_TRUE(config_parse_string(&c,
        "[Gestures]\n"
        "262626262626262=cmd:close_window\n"  /* 15 个字符：可存储 */
        "2626262626262626=key:f5\n"));        /* 16 个字符：超出 CFG_MAX_KEY */
    ASSERT_EQ((int)c.gesture_count, 1);
    ASSERT_STREQ(c.gestures[0].key, "262626262626262");
    ASSERT_EQ(c.diag.invalid_gestures, 1);
    ASSERT_STREQ(c.diag.first_issue, "2626262626262626");
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

UTEST(config, app_gesture_keys_are_normalized_deduplicated_and_validated)
{
    Config c;
    ASSERT_TRUE(config_parse_string(&c,
        "[Gestures]\n6=global\n"
        "[App:chrome.exe]\n"
        "66=first\n"
        "666=last\n"
        "5=invalid\n"));

    const AppConfig *chrome = config_find_app(&c, "chrome.exe");
    ASSERT_TRUE(chrome != NULL);
    ASSERT_EQ((int)chrome->gesture_count, 1);
    ASSERT_STREQ(chrome->gestures[0].key, "6");
    ASSERT_STREQ(chrome->gestures[0].action, "last");
    ASSERT_EQ(c.diag.duplicate_gestures, 1);
    ASSERT_EQ(c.diag.invalid_gestures, 1);

    Pt p[] = {{0,0},{25,0},{50,0},{75,0},{100,0}};
    char key[REC_MAX_SEQ];
    RecMatchResult r;
    ASSERT_STREQ(config_resolve_path(&c, "chrome.exe", p,
                                     sizeof(p) / sizeof(p[0]), key, sizeof(key), &r),
                 "last");
    ASSERT_STREQ(key, "6");
}

UTEST(config, app_section_prefix_case_insensitive)
{
    Config c;
    /* 节名前缀也必须大小写不敏感：此前只认 "App:"/"app:"，[APP:] 会被静默忽略，
     * per-app 覆盖完全失效且无任何诊断。 */
    config_parse_string(&c,
        "[APP:chrome.exe]\n26=key:ctrl+w\n"
        "[ApP:firefox.exe]\n26=key:ctrl+w\n");
    ASSERT_TRUE(config_find_app(&c, "chrome.exe") != NULL);
    ASSERT_TRUE(config_find_app(&c, "firefox.exe") != NULL);
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
    /* 识别器已把视觉 V 归一为 39，PPT 只需显式屏蔽这一条。 */
    ASSERT_TRUE(config_resolve(&c, "powerpnt.exe", "39") == NULL);
    /* 其他程序不受影响 */
    ASSERT_STREQ(config_resolve(&c, "notepad.exe", "39"), "key:f5");
}

UTEST(resolve, down_up_and_v_are_independent)
{
    Config c;
    config_parse_string(&c,
        "[Gestures]\n28=cmd:scroll_bottom\n39=key:f5\n");
    ASSERT_STREQ(config_resolve(&c, "notepad.exe", "28"), "cmd:scroll_bottom");
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

UTEST(resolve, direction_lookup_is_strict)
{
    Config c;
    config_parse_string(&c, "[Gestures]\n26=cmd:close_window\n");
    ASSERT_STREQ(config_resolve(&c, "notepad.exe", "26"), "cmd:close_window");
    ASSERT_TRUE(config_resolve(&c, "notepad.exe", "266") == NULL);
}

UTEST(resolve, raw_path_uses_geometry_and_app_override)
{
    Config c;
    config_parse_string(&c,
        "[Gestures]\n2=cmd:scroll_bottom\n26=cmd:close_window\n"
        "[App:chrome.exe]\n26=key:ctrl+w\n");
    Pt p[] = {{0,0},{0,30},{0,60},{0,100},
              {30,100},{60,100},{100,100}};
    char key[REC_MAX_SEQ];
    RecMatchResult r;
    ASSERT_STREQ(config_resolve_path(&c, "chrome.exe", p,
                                     sizeof(p) / sizeof(p[0]), key, sizeof(key), &r),
                 "key:ctrl+w");
    ASSERT_STREQ(key, "26");
    ASSERT_EQ(r.index, 0); /* app 覆盖项排在有效候选表首位 */
}

UTEST(resolve, raw_path_none_still_reports_a_match)
{
    Config c;
    config_parse_string(&c,
        "[Gestures]\n39=key:f5\n"
        "[App:powerpnt.exe]\n39=cmd:none\n");
    Pt p[] = {{0,0},{20,20},{40,40},{60,60},{80,80},{100,100},
              {120,80},{140,60},{160,40},{180,20},{200,0}};
    char key[REC_MAX_SEQ];
    RecMatchResult r;
    ASSERT_TRUE(config_resolve_path(&c, "powerpnt.exe", p,
                                    sizeof(p) / sizeof(p[0]), key, sizeof(key), &r) == NULL);
    ASSERT_STREQ(key, "39");
    ASSERT_TRUE(r.index >= 0); /* NULL 是显式无动作，不是识别失败 */
}

UTEST(resolve, unfinished_dangerous_v_never_executes_diagonal_prefix)
{
    Config c;
    config_parse_string(&c,
        "[Gestures]\n"
        "3=key:delete\n"
        "39=key:f5\n");

    /* 第一臂约 990px，转向确认阈值约 119px；60px 的反向尾巴只算意图。 */
    Pt unfinished[] = {{0,0},{140,140},{280,280},{420,420},{560,560},{700,700},
                       {714,686},{728,672},{742,658}};
    char key[REC_MAX_SEQ];
    RecMatchResult r;
    ASSERT_TRUE(config_resolve_path(&c, "notepad.exe", unfinished,
                                    sizeof(unfinished) / sizeof(unfinished[0]),
                                    key, sizeof(key), &r) == NULL);
    ASSERT_EQ(r.index, -1);

    /* 超过 120px 后第二臂确认，完整 V 才允许执行 F5。 */
    Pt finished[] = {{0,0},{140,140},{280,280},{420,420},{560,560},{700,700},
                     {730,670},{760,640},{790,610}};
    ASSERT_STREQ(config_resolve_path(&c, "notepad.exe", finished,
                                     sizeof(finished) / sizeof(finished[0]),
                                     key, sizeof(key), &r),
                 "key:f5");
    ASSERT_STREQ(key, "39");
}

/* ---------------- 布尔值回落（与数值的 fallback_clamp 同一哲学） ---------------- */

/*
 * 无法解析的布尔值必须回落到该字段的文档默认值，而不是一律 false。
 * 数值字段早就这么做了（`MinDistance = 二十` → 回落 20，见
 * garbage_text_falls_back_to_defaults），布尔字段此前却把一切无法识别的输入
 * 翻成 false —— 而受害的五个字段默认全是 true。后果最重的是
 * `DisableOnFullscreen = ture`（手误）：全屏门控被静默关掉，手势会在全屏游戏里误触发。
 */
UTEST(config, garbage_bool_falls_back_to_default)
{
    Config c;
    const char *ini =
        "[General]\n"
        "ShowTrail = \xE7\x9C\x9F\n"            /* 真 */
        "DisableOnFullscreen = ture\n"          /* true 的手误 */
        "RestoreEvent = enable\n"
        "TrailArrow = \xE6\x98\xAF\n"           /* 是 */
        "[App:game.exe]\n"
        "Enabled = enabled\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_TRUE(c.show_trail);
    ASSERT_TRUE(c.disable_on_fullscreen);
    ASSERT_TRUE(c.restore_event);
    ASSERT_TRUE(c.trail_arrow);
    const AppConfig *game = config_find_app(&c, "game.exe");
    ASSERT_TRUE(game != NULL);
    ASSERT_TRUE(game->enabled);      /* 默认启用；只有明确的 false 才禁用 */
    ASSERT_EQ(c.diag.bad_values, 5);
}

/* 默认为 false 的字段同样回落到 false（防止「回落」被误实现成一律 true）。 */
UTEST(config, garbage_bool_falls_back_to_false_when_default_is_false)
{
    Config c;
    ASSERT_TRUE(config_parse_string(&c, "[General]\nAutoStart=ture\nRandomColor=\xE7\x9C\x9F\n"));
    ASSERT_FALSE(c.auto_start);
    ASSERT_FALSE(c.random_color);
}

/* 防回归：合法写法必须原样生效，不能被回落逻辑吃掉。 */
UTEST(config, well_formed_bools_are_unaffected)
{
    Config c;
    const char *ini =
        "[General]\n"
        "ShowTrail = false\n"
        "DisableOnFullscreen = off\n"
        "RestoreEvent = 0\n"
        "TrailArrow = no\n"
        "RandomColor = TRUE\n"
        "AutoStart = Yes\n"
        "[App:game.exe]\n"
        "Enabled = false\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_FALSE(c.show_trail);
    ASSERT_FALSE(c.disable_on_fullscreen);
    ASSERT_FALSE(c.restore_event);
    ASSERT_FALSE(c.trail_arrow);
    ASSERT_TRUE(c.random_color);
    ASSERT_TRUE(c.auto_start);
    ASSERT_FALSE(config_find_app(&c, "game.exe")->enabled);
    ASSERT_EQ(c.diag.bad_values, 0);
}

/* ---------------- 解析诊断 ---------------- */

/* [General] 里拼错的键此前被静默忽略：设置永远不生效，日志一片祥和。 */
UTEST(config, unknown_key_is_reported)
{
    Config c;
    ASSERT_TRUE(config_parse_string(&c, "[General]\nTrailWith = 5\nTorlerance = 1\n"));
    ASSERT_EQ(c.diag.unknown_keys, 2);
    ASSERT_STREQ(c.diag.first_issue, "TrailWith");
    ASSERT_EQ(c.trail_width, 3);        /* 未被误写 */
}

UTEST(config, known_keys_are_not_reported_as_unknown)
{
    Config c;
    ASSERT_TRUE(config_parse_string(&c,
        "[General]\nTrailWidth = 5\nMatchScore = 84\nAmbiguityMargin = 7\n"));
    ASSERT_EQ(c.diag.unknown_keys, 0);
    ASSERT_EQ(c.trail_width, 5);
    ASSERT_EQ(c.match_score, 84);
    ASSERT_EQ(c.ambiguity_margin, 7);
}

/* 容量上限撞满后此前静默丢弃：第 129 条起的手势永不生效且无从排查。 */
UTEST(config, gesture_overflow_is_reported)
{
    static char ini[CFG_MAX_GESTURES * 40 + 256];
    static const char digit[] = {'4', '6', '8', '1'};
    int off = snprintf(ini, sizeof(ini), "[Gestures]\n");
    for (int i = 0; i < CFG_MAX_GESTURES + 5; i++) {
        char key[10];
        snprintf(key, sizeof(key), "2%c2%c2%c2%c",
                 digit[(i >> 0) & 3], digit[(i >> 2) & 3],
                 digit[(i >> 4) & 3], digit[(i >> 6) & 3]);
        off += snprintf(ini + off, sizeof(ini) - (size_t)off,
                        "%s = cmd:minimize\n", key);
    }
    static Config c;
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ((int)c.gesture_count, CFG_MAX_GESTURES);
    ASSERT_EQ(c.diag.dropped, 5);
}

UTEST(config, app_overflow_is_reported)
{
    static char ini[CFG_MAX_APPS * 48 + 256];
    int off = 0;
    ini[0] = '\0';
    for (int i = 0; i < CFG_MAX_APPS + 3; i++)
        off += snprintf(ini + off, sizeof(ini) - (size_t)off,
                        "[App:a%03d.exe]\n26 = cmd:minimize\n", i);
    static Config c;
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ((int)c.app_count, CFG_MAX_APPS);
    ASSERT_TRUE(c.diag.dropped >= 3);
}

/* 干净配置不得报出任何诊断（否则日志会天天喊狼来了）。 */
UTEST(config, clean_config_reports_nothing)
{
    Config c;
    const char *ini =
        "[General]\nTrigger = right\nMinDistance = 20\nShowTrail = true\n"
        "[Gestures]\n26 = cmd:close_window\n"
        "[App:chrome.exe]\n26 = key:ctrl+w\n";
    ASSERT_TRUE(config_parse_string(&c, ini));
    ASSERT_EQ(c.diag.dropped, 0);
    ASSERT_EQ(c.diag.unknown_keys, 0);
    ASSERT_EQ(c.diag.bad_values, 0);
    ASSERT_EQ(c.diag.invalid_gestures, 0);
    ASSERT_EQ(c.diag.duplicate_gestures, 0);
    ASSERT_STREQ(c.diag.first_issue, "");
}

UTEST_MAIN();
