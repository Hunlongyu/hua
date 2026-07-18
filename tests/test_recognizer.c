/*
 * test_recognizer.c —— recognizer 纯逻辑单元测试（utest.h）。
 */
#include "recognizer.h"
#include "utest.h"

#include <math.h>
#include <stdio.h>

#define NELEMS(a) (sizeof(a) / sizeof((a)[0]))

/*
 * 生成 V 形轨迹：两臂与水平线夹角 deg、深 depth，每臂 50 个采样点。
 * jitter>0 时叠加 ±jitter 像素的确定性伪随机噪声（自带 LCG，不用 rand()，
 * 保证测试可复现）。
 */
static size_t make_v(Pt *p, size_t cap, double deg, double depth, int jitter)
{
    const double PI = 3.14159265358979323846;
    double halfw = depth / tan(deg * PI / 180.0);
    unsigned s = 12345u;
    size_t n = 0;

    for (int arm = 0; arm < 2; arm++) {
        for (int i = (arm ? 1 : 0); i <= 50 && n < cap; i++) {
            double t = i / 50.0;
            double x = arm ? halfw + t * halfw : t * halfw;
            double y = arm ? depth - t * depth : t * depth;
            int jx = 0, jy = 0;
            if (jitter > 0) {
                s = s * 1103515245u + 12345u;
                jx = (int)((s >> 16) % (unsigned)(2 * jitter + 1)) - jitter;
                s = s * 1103515245u + 12345u;
                jy = (int)((s >> 16) % (unsigned)(2 * jitter + 1)) - jitter;
            }
            p[n].x = (int)x + jx;
            p[n].y = (int)y + jy;
            n++;
        }
    }
    return n;
}

/*
 * 生成任意朝向的 V：先造标准 V（顶点在下、开口向右上，编为 39），再按
 * mirror_x / mirror_y 镜像，得到另外三个朝向。无抖动，用于验证朝向覆盖。
 *   mirror=(0,0) ↓↗ → 39   (1,0) ↓↖ → 17
 *   mirror=(0,1) ↑↘ → 93   (1,1) ↑↙ → 71
 */
static size_t make_v_oriented(Pt *p, size_t cap, double deg, double depth,
                              int mirror_x, int mirror_y)
{
    const double PI = 3.14159265358979323846;
    double halfw = depth / tan(deg * PI / 180.0);
    size_t n = 0;

    for (int arm = 0; arm < 2; arm++) {
        for (int i = (arm ? 1 : 0); i <= 50 && n < cap; i++) {
            double t = i / 50.0;
            double x = arm ? halfw + t * halfw : t * halfw;
            double y = arm ? depth - t * depth : t * depth;
            p[n].x = (int)(mirror_x ? -x : x);
            p[n].y = (int)(mirror_y ? -y : y);
            n++;
        }
    }
    return n;
}

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

/* ---------------- rec_encode：V 形（配置里 39 = key:f5） ---------------- */

/*
 * V 的两臂只要落在 22.5°~67.5° 这一扇区内就该编成 "39"，与"画得多宽"无关。
 * 曾经的行为：拐弯时锚点还留在下降臂上，等位移攒够 min_dist，锚点→当前点的净
 * 向量已经几乎水平，于是顶点凭空多出一段"右"(6)，输出 "369"——永远匹配不上 39。
 * 而拐角伪影过滤只删「夹在两个正交分量之间的短对角段」（2→3→6 删 3），对
 * 「夹在两个对角之间的短正交段」（3→6→9 删 6）视而不见，正好漏掉 V 的顶点。
 * 结果是：越把 V 画成教科书式的 45°，越是必挂。
 */
UTEST(encode, v_wide_is_39)
{
    Pt p[256];
    char buf[64], msg[64];
    for (double deg = 30; deg <= 65; deg += 5) {
        size_t n = make_v(p, NELEMS(p), deg, 200, 0);
        rec_encode(p, n, 20, buf, sizeof(buf));
        snprintf(msg, sizeof(msg), "V 臂角 %.0f 度应编为 39", deg);
        ASSERT_STREQ_MSG(buf, "39", msg);
    }
}

/* 真正近似沿同一条竖线下、上的 28 必须保持独立，不能被 V 形规则吞掉。 */
UTEST(encode, down_up_with_small_drift_is_28)
{
    Pt p[] = {
        {0,0},{2,20},{4,40},{6,60},{8,80},{10,100},
        {12,80},{14,60},{16,40},{18,20},{20,0}
    };
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "28");
}

/* 2026-07-17 用户日志中的真实 V：右臂虽陡，但整体明显向右展开，应按人眼认成 39。 */
UTEST(encode, v_from_user_log_is_39)
{
    Pt p[] = {
        {2546,357},{2551,375},{2562,399},{2569,419},{2575,430},{2579,442},
        {2585,453},{2593,472},{2607,498},{2617,514},{2623,525},{2629,536},
        {2637,551},{2645,562},{2654,574},{2667,591},{2676,601},{2684,612},
        {2692,621},{2694,605},{2697,590},{2703,558},{2709,529},{2712,517},
        {2717,495},{2721,479},{2727,460},{2734,436},{2738,422},{2743,404},
        {2745,392},{2750,372},{2754,360},{2758,345},{2762,329},{2765,317}
    };
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "39");
}

/* 向下后向右的关闭手势允许末段略微上扬；它只有几像素的“升高”，不构成 V。 */
UTEST(encode, L_down_then_right_with_slight_rise_is_26)
{
    Pt p[] = {
        {0,0},{2,20},{3,40},{4,60},{6,80},{8,100},{10,120},
        {30,119},{50,118},{70,117},{90,116},{110,115},{130,114},{160,113}
    };
    char buf[64];
    rec_encode(p, NELEMS(p), 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "26");
}

/*
 * 贴着扇区边界（67.5°）的笔画不许被手抖甩碎。
 * 曾经的行为：每 min_dist 独立量化一次方向，65° 的臂只要有 ±2px 手抖就在
 * 3(右下)/2(下) 之间来回翻面，编出 "3232398989" 这种碎串——用户日志里的
 * "3239898"/"2398989"/"2398" 全是它。改为按整段净位移量化后，结果由整笔走向
 * 决定，手抖被平均掉。
 */
UTEST(encode, v_near_boundary_is_jitter_stable)
{
    Pt p[256];
    char buf[64];
    size_t n = make_v(p, NELEMS(p), 60, 200, 2);
    rec_encode(p, n, 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "39");

    n = make_v(p, NELEMS(p), 65, 200, 2);
    rec_encode(p, n, 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "39");

    n = make_v(p, NELEMS(p), 75, 200, 2);
    rec_encode(p, n, 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "39");
}

/*
 * V 的归一必须对四个朝向对称，尤其在陡角（≥70°）——扇区量化本会把陡臂拽向深度
 * 轴，旧实现只硬编码了 ↓↗→39 一个朝向，另三个朝向在陡角退化：↓↖ 退成 28、
 * ↑↘ 与 ↑↙ 双双退成 82（两个肉眼不同的手势撞成同串，无法分别绑定）。
 * 修复后每个朝向都稳定归一到对应的对角对。
 */
UTEST(encode, v_all_orientations_steep)
{
    Pt p[256];
    char buf[64], msg[80];
    struct { int mx, my; const char *want; } o[4] = {
        {0, 0, "39"}, {1, 0, "17"}, {0, 1, "93"}, {1, 1, "71"},
    };
    for (double deg = 30; deg <= 80; deg += 5) {
        for (int k = 0; k < 4; k++) {
            size_t n = make_v_oriented(p, NELEMS(p), deg, 200, o[k].mx, o[k].my);
            rec_encode(p, n, 20, buf, sizeof(buf));
            snprintf(msg, sizeof(msg), "V 朝向(mx=%d,my=%d) %.0f度 应编为 %s",
                     o[k].mx, o[k].my, deg, o[k].want);
            ASSERT_STREQ_MSG(buf, o[k].want, msg);
        }
    }
}

/*
 * 「先竖直向下、再对角上行」的分界不该由 1 像素决定。旧实现要求 dx1>0，于是
 * 第一臂恰好竖直（29）与右漂 1px（被判成 39）之间硬翻面。改用 min_dist(20px)
 * 死区后：横向张开不足 20px 的仍是竖直下上，保持 29；真正张开的才算 V。
 */
UTEST(encode, v_vertical_first_arm_needs_real_opening)
{
    char buf[64];
    /* 第一臂纯竖直向下 200，再 45° 右上：横向全靠第二臂，第一臂无张开 → 29。 */
    Pt a[128]; size_t na = 0;
    for (int i = 0; i <= 50; i++) { a[na].x = 0;              a[na].y = i * 4; na++; }
    for (int i = 1; i <= 50; i++) { a[na].x = i * 4; a[na].y = 200 - i * 4; na++; }
    rec_encode(a, na, 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "29");

    /* 第一臂右漂仅 10px（< min_dist）：仍视作竖直，保持 29，不因微小漂移翻成 V。 */
    Pt b[128]; size_t nb = 0;
    for (int i = 0; i <= 50; i++) { b[nb].x = i / 5;          b[nb].y = i * 4; nb++; }
    for (int i = 1; i <= 50; i++) { b[nb].x = 10 + i * 4; b[nb].y = 200 - i * 4; nb++; }
    rec_encode(b, nb, 20, buf, sizeof(buf));
    ASSERT_STREQ(buf, "29");
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

/*
 * 空串不是手势，任何 Tolerance 下都不许命中。
 *
 * 曾经的行为：lev("", "1") == 1，故 Tolerance>=1 时空串命中任意单段模板，
 * 且「平票取短 + 取前」保证必中 [Gestures] 里第一条单字符手势（发布配置中是
 * `1 = cmd:minimize`）。触发它不需要划手势——TriggerDistance(5) 就进 Active，
 * 而分段要到 MinDistance(20)，中间这段「死区」里 rec_encode 产出的正是空串。
 * 于是一次手抖的右键点击就会把当前窗口最小化。
 * 容错的语义是「画歪了也认」，不是「没画也认」。
 */
UTEST(match, empty_seq_never_matches)
{
    const char *keys[] = {"1", "2", "26"};
    /* 上界取 config.h 的 CFG_MAX_TOLERANCE(=4)；此处不引 config.h，保持
     * recognizer 测试与 config 模块解耦。 */
    for (int tol = 0; tol <= 4; tol++)
        ASSERT_EQ(rec_match("", keys, 3, tol), -1);
}

UTEST(match, null_seq_never_matches)
{
    const char *keys[] = {"1", "2"};
    ASSERT_EQ(rec_match(NULL, keys, 2, 1), -1);
}

UTEST_MAIN();
