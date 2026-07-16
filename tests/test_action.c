/*
 * test_action.c —— 快捷键描述解析（action_parse_key）单元测试（utest.h）。
 * 依赖 windows.h 的 VK_* 常量；本项目本就只针对 Windows。
 */
#include "action.h"
#include "utest.h"

UTEST(parse_key, single_letter)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("t", &k));
    ASSERT_FALSE(k.ctrl);
    ASSERT_FALSE(k.alt);
    ASSERT_FALSE(k.shift);
    ASSERT_FALSE(k.win);
    ASSERT_EQ(k.vk, 'T');   /* 字母主键 VK = 大写 ASCII */
}

UTEST(parse_key, ctrl_letter)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("ctrl+t", &k));
    ASSERT_TRUE(k.ctrl);
    ASSERT_FALSE(k.alt);
    ASSERT_EQ(k.vk, 'T');
}

UTEST(parse_key, ctrl_shift_letter)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("ctrl+shift+t", &k));
    ASSERT_TRUE(k.ctrl);
    ASSERT_TRUE(k.shift);
    ASSERT_FALSE(k.alt);
    ASSERT_EQ(k.vk, 'T');
}

UTEST(parse_key, case_insensitive)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("CTRL+Shift+T", &k));
    ASSERT_TRUE(k.ctrl);
    ASSERT_TRUE(k.shift);
    ASSERT_EQ(k.vk, 'T');
}

UTEST(parse_key, alt_named_arrow)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("alt+left", &k));
    ASSERT_TRUE(k.alt);
    ASSERT_EQ(k.vk, VK_LEFT);
}

UTEST(parse_key, function_key)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("f5", &k));
    ASSERT_EQ(k.vk, VK_F5);
    ASSERT_TRUE(action_parse_key("f24", &k));
    ASSERT_EQ(k.vk, VK_F24);
}

UTEST(parse_key, function_key_rejects_trailing_garbage)
{
    KeyCombo k;
    /* atoi 会忽略尾部垃圾：不整串校验的话 "f12abc" 会被静默当成 F12。 */
    ASSERT_FALSE(action_parse_key("f12abc", &k));
    ASSERT_FALSE(action_parse_key("f5x", &k));
    ASSERT_FALSE(action_parse_key("f0", &k));    /* 越界：f1..f24 */
    ASSERT_FALSE(action_parse_key("f25", &k));
}

UTEST(parse_key, named_esc)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("esc", &k));
    ASSERT_EQ(k.vk, VK_ESCAPE);
}

UTEST(parse_key, digit)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("ctrl+1", &k));
    ASSERT_TRUE(k.ctrl);
    ASSERT_EQ(k.vk, '1');
}

UTEST(parse_key, win_key)
{
    KeyCombo k;
    ASSERT_TRUE(action_parse_key("win+d", &k));
    ASSERT_TRUE(k.win);
    ASSERT_EQ(k.vk, 'D');
}

UTEST(parse_key, empty_is_invalid)
{
    KeyCombo k;
    ASSERT_FALSE(action_parse_key("", &k));
}

UTEST(parse_key, only_modifier_is_invalid)
{
    KeyCombo k;
    ASSERT_FALSE(action_parse_key("ctrl+", &k));
    ASSERT_FALSE(action_parse_key("ctrl", &k));
}

UTEST_MAIN();
