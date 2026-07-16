/*
 * test_recognizer.c —— recognizer 纯逻辑单元测试（utest.h）。
 */
#include "recognizer.h"
#include "utest.h"

#define NELEMS(a) (sizeof(a) / sizeof((a)[0]))

/* ---------------- rec_encode：直线各方向 ---------------- */

UTEST(encode, straight_right)
{
    Pt p[] = {{0,0},{10,0},{20,0},{30,0},{40,0},{60,0},{80,0},{100,0}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "6");
}

UTEST(encode, straight_left)
{
    Pt p[] = {{0,0},{-20,0},{-40,0},{-60,0},{-100,0}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "4");
}

UTEST(encode, straight_down)
{
    /* 屏幕坐标 y 向下 = 方向 2。 */
    Pt p[] = {{0,0},{0,20},{0,40},{0,60},{0,100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "2");
}

UTEST(encode, straight_up)
{
    Pt p[] = {{0,0},{0,-20},{0,-40},{0,-60},{0,-100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "8");
}

UTEST(encode, diag_right_down)
{
    Pt p[] = {{0,0},{20,20},{40,40},{60,60},{100,100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "3");
}

UTEST(encode, diag_right_up)
{
    Pt p[] = {{0,0},{20,-20},{40,-40},{100,-100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "9");
}

UTEST(encode, diag_left_down)
{
    Pt p[] = {{0,0},{-20,20},{-40,40},{-100,100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "1");
}

UTEST(encode, diag_left_up)
{
    Pt p[] = {{0,0},{-20,-20},{-40,-40},{-100,-100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "7");
}

/* ---------------- rec_encode：多段 ---------------- */

UTEST(encode, L_right_then_down)
{
    Pt p[] = {{0,0},{30,0},{60,0},{100,0},
              {100,30},{100,60},{100,100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "62");
}

UTEST(encode, Z_right_down_right)
{
    Pt p[] = {{0,0},{50,0},{100,0},
              {100,50},{100,100},
              {150,100},{200,100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "626");
}

UTEST(encode, L_corner_no_diagonal_artifact)
{
    /* 竖直段长 50（非 min_dist 整数倍），拐角锚点落在角上方，旧算法会在拐角
       产生一段斜向 "3"，得到 "236"；正确应为 "26"。 */
    Pt p[] = {{0,0},{0,10},{0,20},{0,30},{0,40},{0,50},
              {10,50},{20,50},{30,50},{40,50},{50,50},{60,50}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "26");
}

UTEST(encode, long_diagonal_is_kept)
{
    /* 真正的长对角手势（右下 3）不能被当成伪影删掉。 */
    Pt p[] = {{0,0},{20,20},{40,40},{60,60},{80,80},{100,100}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "3");
}

/* ---------------- rec_encode：抗抖动 ---------------- */

UTEST(encode, below_threshold_is_empty)
{
    Pt p[] = {{0,0},{5,0},{8,0},{3,0}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "");
}

UTEST(encode, small_jitter_absorbed)
{
    Pt p[] = {{0,0},{10,5},{20,-5},{30,5},{40,0},{50,-5},{60,0},{80,3},{100,0}};
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "6");
}

/* ---------------- rec_levenshtein ---------------- */

UTEST(levenshtein, equal)
{
    ASSERT_EQ(rec_levenshtein("", ""), 0);
    ASSERT_EQ(rec_levenshtein("abc", "abc"), 0);
}

UTEST(levenshtein, empty)
{
    ASSERT_EQ(rec_levenshtein("", "abc"), 3);
    ASSERT_EQ(rec_levenshtein("abc", ""), 3);
}

UTEST(levenshtein, single_edits)
{
    ASSERT_EQ(rec_levenshtein("26", "2"),   1);   /* 删 */
    ASSERT_EQ(rec_levenshtein("26", "28"),  1);   /* 改 */
    ASSERT_EQ(rec_levenshtein("26", "246"), 1);   /* 增 */
}

UTEST(levenshtein, classic)
{
    ASSERT_EQ(rec_levenshtein("kitten", "sitting"), 3);
}

/* ---------------- rec_match ---------------- */

UTEST(match, exact)
{
    const char *keys[] = {"2", "26", "4"};
    ASSERT_EQ(rec_match("26", keys, 3, 0), 1);
}

UTEST(match, exact_none)
{
    const char *keys[] = {"2", "26"};
    ASSERT_EQ(rec_match("8", keys, 2, 0), -1);
}

UTEST(match, fuzzy_picks_nearest)
{
    const char *keys[] = {"26", "2"};   /* "266": 距"26"=1, 距"2"=2 */
    ASSERT_EQ(rec_match("266", keys, 2, 1), 0);
}

UTEST(match, tie_prefers_shorter)
{
    const char *a[] = {"264", "2"};     /* 距各=1；更短 "2" 胜 */
    ASSERT_EQ(rec_match("26", a, 2, 1), 1);
    const char *b[] = {"2", "264"};     /* 换序，更短者仍胜 */
    ASSERT_EQ(rec_match("26", b, 2, 1), 0);
}

UTEST(match, tie_equal_len_prefers_earlier)
{
    const char *keys[] = {"6", "2"};    /* 距各=1、同长 → 取更前 */
    ASSERT_EQ(rec_match("26", keys, 2, 1), 0);
}

UTEST_MAIN();
