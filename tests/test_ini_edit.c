/*
 * test_ini_edit.c —— ini_set_autostart 纯逻辑单元测试（utest.h）。
 * 钉住 ini_write_autostart 历史上踩过的四个坑：嵌入 NUL、AutoStart 前缀误匹配、
 * 无节感知追加、以及基本的就地替换 / 插入 / 补节三种路径。
 */
#include "ini_edit.h"
#include "utest.h"

#include <string.h>

/* 便捷封装：对 C 字符串输入调用，返回结果串（写入调用方 buf）。 */
static size_t run(const char *in, bool enable, char *out, size_t cap)
{
    return ini_set_autostart(in, strlen(in), enable, out, cap);
}

/* ---------------- 就地替换 ---------------- */

UTEST(ini, replace_existing_true_to_false)
{
    char out[256];
    size_t n = run("[General]\r\nAutoStart       = true\r\nTrigger = right\r\n",
                   false, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out,
        "[General]\r\nAutoStart       = false\r\nTrigger = right\r\n");
}

UTEST(ini, replace_existing_false_to_true)
{
    char out[256];
    size_t n = run("[General]\r\nAutoStart       = false\r\n",
                   true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out, "[General]\r\nAutoStart       = true\r\n");
}

/* 行尾注释在替换时不保留——沿用旧实现的行为，此测试用于锁定它。 */
UTEST(ini, replace_drops_trailing_comment)
{
    char out[256];
    size_t n = run("[General]\r\nAutoStart       = false      ; 开机自启\r\n",
                   true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out, "[General]\r\nAutoStart       = true\r\n");
}

/* 缩进的 AutoStart（前导空白）仍应被识别并替换。 */
UTEST(ini, replace_indented_key)
{
    char out[256];
    size_t n = run("[General]\r\n  AutoStart = false\r\n", true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out, "[General]\r\nAutoStart       = true\r\n");
}

/* 大小写不敏感：[general] / autostart 都要认。 */
UTEST(ini, case_insensitive)
{
    char out[256];
    size_t n = run("[general]\r\nautostart = false\r\n", true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out, "[general]\r\nAutoStart       = true\r\n");
}

/* ---------------- 插入（有 [General] 无 AutoStart）---------------- */

UTEST(ini, insert_after_general_header)
{
    char out[256];
    size_t n = run("[General]\r\nTrigger = right\r\n", false, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out,
        "[General]\r\nAutoStart       = false\r\nTrigger = right\r\n");
}

/* [General] 头恰是最后一行且无换行：插入前补一个换行。补的是裸 \n（沿用旧实现，
 * 该行原本就没有 \r 可保留），随后的插入行本身仍是 CRLF。 */
UTEST(ini, insert_when_header_is_last_line_no_newline)
{
    char out[256];
    size_t n = run("[General]", true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out, "[General]\nAutoStart       = true\r\n");
}

/* ---------------- 补节（无 [General]）---------------- */

UTEST(ini, append_section_when_no_general)
{
    char out[256];
    size_t n = run("[App:foo.exe]\r\n26 = key:f5\r\n", true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out,
        "[App:foo.exe]\r\n26 = key:f5\r\n"
        "\r\n[General]\r\nAutoStart       = true\r\n");
}

/*
 * 关键回归：AutoStart 出现在 [App:] 节里（不是 [General]）时，绝不能就地改它——
 * 那是某程序的一条手势键。应把真正的开关插进 [General]（若存在）。
 */
UTEST(ini, autostart_inside_app_section_not_touched)
{
    char out[256];
    size_t n = run("[General]\r\nTrigger = right\r\n"
                   "[App:foo.exe]\r\nAutoStart = 1\r\n",
                   true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out,
        "[General]\r\nAutoStart       = true\r\nTrigger = right\r\n"
        "[App:foo.exe]\r\nAutoStart = 1\r\n");
}

/*
 * 关键回归：AutoStartFoo 这类前缀相同的键不是 AutoStart，不能被改写；
 * 且因为没有真正的 AutoStart，应插入一行新的。
 */
UTEST(ini, prefix_key_not_matched)
{
    char out[256];
    size_t n = run("[General]\r\nAutoStartFoo = 1\r\n", true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out,
        "[General]\r\nAutoStart       = true\r\nAutoStartFoo = 1\r\n");
}

/* ---------------- 拒绝 / 边界 ---------------- */

/* 嵌入 NUL（in_len > strlen）：拒绝，返回 FAIL。 */
UTEST(ini, embedded_nul_refused)
{
    char out[256];
    const char in[] = "[General]\r\nAutoStart = false\r\n\0trailing";
    size_t in_len = sizeof(in) - 1;   /* 含 NUL 之后的字节 */
    size_t n = ini_set_autostart(in, in_len, true, out, sizeof(out));
    ASSERT_EQ(n, HUA_INI_EDIT_FAIL);
}

/* 输出缓冲不足：返回 FAIL，绝不越界写。 */
UTEST(ini, output_too_small_fails)
{
    char out[8];
    size_t n = run("[General]\r\nAutoStart = false\r\n", true, out, sizeof(out));
    ASSERT_EQ(n, HUA_INI_EDIT_FAIL);
}

/* 空输入：无 [General]，补一个完整节。 */
UTEST(ini, empty_input_appends_section)
{
    char out[128];
    size_t n = run("", false, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_STREQ(out, "\r\n[General]\r\nAutoStart       = false\r\n");
}

/* LF-only（无 \r）输入也要正确处理，不强加 CRLF 到原有行。 */
UTEST(ini, lf_only_lines_preserved)
{
    char out[256];
    size_t n = run("[General]\nAutoStart = false\n", true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    /* 原有行的 \n 保留；替换行本身按约定写成 CRLF。 */
    ASSERT_STREQ(out, "[General]\nAutoStart       = true\r\n");
}

/* 返回值等于结果串长度。 */
UTEST(ini, return_value_is_length)
{
    char out[256];
    size_t n = run("[General]\r\nAutoStart       = true\r\n", true, out, sizeof(out));
    ASSERT_NE(n, HUA_INI_EDIT_FAIL);
    ASSERT_EQ(n, strlen(out));
}

UTEST_MAIN();
