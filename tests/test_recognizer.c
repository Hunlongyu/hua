/*
 * test_recognizer.c —— recognizer 纯逻辑单元测试（utest.h）。
 */
#include "recognizer.h"
#include "utest.h"

#include <limits.h>
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

static double direction_angle(char c)
{
    const double PI = 3.14159265358979323846;
    switch (c) {
    case '6': return 0.0;
    case '3': return PI / 4.0;
    case '2': return PI / 2.0;
    case '1': return 3.0 * PI / 4.0;
    case '4': return PI;
    case '7': return -3.0 * PI / 4.0;
    case '8': return -PI / 2.0;
    default:  return -PI / 4.0; /* 9 */
    }
}

/* 从方向 key 生成不等长、不等密度且带像素噪声的确定性轨迹。 */
static size_t make_key_path(Pt *p, size_t cap, const char *key,
                            int base_length, int max_angle_error_deg,
                            int noise, unsigned seed)
{
    const double PI = 3.14159265358979323846;
    double start_x = 500.0, start_y = 500.0;
    size_t n = 0;
    if (cap)
        p[n++] = (Pt){(int)start_x, (int)start_y};

    for (size_t segment = 0; key[segment] && n < cap; segment++) {
        seed = seed * 1103515245u + 12345u;
        int length_pct = 65 + (int)((seed >> 16) % 91u); /* 65%..155% */
        double length = (double)base_length * (double)length_pct / 100.0;
        seed = seed * 1103515245u + 12345u;
        int error = max_angle_error_deg
                        ? (int)((seed >> 16) % (unsigned)(2 * max_angle_error_deg + 1))
                              - max_angle_error_deg
                        : 0;
        double angle = direction_angle(key[segment]) + (double)error * PI / 180.0;
        seed = seed * 1103515245u + 12345u;
        int steps = 12 + (int)((seed >> 16) % 20u);
        double end_x = start_x + cos(angle) * length;
        double end_y = start_y + sin(angle) * length;

        for (int i = 1; i <= steps && n < cap; i++) {
            double t = (double)i / (double)steps;
            int jx = 0, jy = 0;
            if (noise && i != steps) {
                seed = seed * 1103515245u + 12345u;
                jx = (int)((seed >> 16) % (unsigned)(2 * noise + 1)) - noise;
                seed = seed * 1103515245u + 12345u;
                jy = (int)((seed >> 16) % (unsigned)(2 * noise + 1)) - noise;
            }
            double x = start_x + (end_x - start_x) * t;
            double y = start_y + (end_y - start_y) * t;
            p[n].x = (int)(x >= 0.0 ? x + 0.5 : x - 0.5) + jx;
            p[n].y = (int)(y >= 0.0 ? y + 0.5 : y - 0.5) + jy;
            n++;
        }
        start_x = end_x;
        start_y = end_y;
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

    const char *keys[] = {"28", "39", "29"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 1);
    ASSERT_TRUE(r.score >= 82);
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

/* ---------------- 模板标准化 ---------------- */

UTEST(template, normalize_and_validate)
{
    char key[REC_MAX_SEQ + 1];
    ASSERT_EQ((int)rec_normalize_template("266699", key, sizeof(key)), 3);
    ASSERT_STREQ(key, "269");
    ASSERT_EQ((int)rec_normalize_template("25", key, sizeof(key)), 0);
    ASSERT_STREQ(key, "");
    ASSERT_EQ((int)rec_normalize_template("", key, sizeof(key)), 0);
}

UTEST(template, insufficient_output_buffer_is_rejected_atomically)
{
    char key[3] = {'x', 'x', '\0'};
    ASSERT_EQ((int)rec_normalize_template("262", key, sizeof(key)), 0);
    ASSERT_STREQ(key, "");
}

/* ---------------- 连续几何匹配 ---------------- */

UTEST(match_path, canonical_L_selects_26)
{
    Pt p[] = {{0,0},{0,25},{0,50},{0,75},{0,100},
              {25,100},{50,100},{75,100},{100,100}};
    const char *keys[] = {"2", "26", "6", "3"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 1);
    ASSERT_EQ(r.index, 1);
    ASSERT_TRUE(r.score >= 95);
    ASSERT_TRUE(r.score - r.second_score >= 6);
}

UTEST(match_path, uneven_sampling_and_scale_do_not_change_result)
{
    /* 同一个下右手势：第一段采样密、第二段采样稀，且两臂长度不相等。 */
    Pt p[] = {{400,300},{401,310},{399,322},{402,335},{400,350},{401,365},
              {399,382},{400,400},{430,401},{470,399},{520,400},{600,401}};
    const char *keys[] = {"2", "26", "3", "6"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 1);
    ASSERT_TRUE(r.score >= 85);
}

UTEST(match_path, adaptive_turn_rejects_short_terminal_hook)
{
    Pt p[] = {{0,0},{0,40},{0,80},{0,120},{0,160},{0,200},
              {30,200},{60,200},{90,200},{120,200},
              {121,210},{120,218}}; /* 长横线末端 18px 下钩，不应产生第三段 */
    const char *keys[] = {"26", "262", "2"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 0);
}

UTEST(match_path, unfinished_turn_does_not_execute_prefix)
{
    Pt p[] = {{0,0},{0,40},{0,80},{0,120},{0,160},{0,200},
              {8,200},{16,200},{25,200}}; /* 转向阈值40；25px已像转向但尚未确认 */
    const char *keys[] = {"2", "26"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), -1);
    ASSERT_EQ(r.index, -1);
}

UTEST(match_path, adaptive_turn_threshold_matrix)
{
    const char *keys[] = {"2", "26"};
    const int tails[] = {0, 19, 20, 25, 39, 40};
    const int expected[] = {0, 0, -1, -1, -1, 1};
    char msg[96];

    for (size_t i = 0; i < NELEMS(tails); i++) {
        Pt p[] = {{0,0},{0,40},{0,80},{0,120},{0,160},{0,200},
                  {tails[i],200}};
        RecMatchResult r;
        int got = rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r);
        snprintf(msg, sizeof(msg), "首段200px、末段%dpx，应命中候选索引%d",
                 tails[i], expected[i]);
        ASSERT_EQ_MSG(got, expected[i], msg);
    }
}

UTEST(match_path, long_first_leg_short_turn_never_executes_prefix)
{
    /* 旧逻辑：确认阈值为 120px，60px 又没到其 60%，被静默删除并以 100 分执行 2。 */
    Pt p[] = {{0,0},{0,100},{0,200},{0,300},{0,400},{0,500},
              {0,600},{0,700},{0,800},{0,900},{0,1000},
              {20,1000},{40,1000},{60,1000}};
    const char *keys[] = {"2", "26"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), -1);
    ASSERT_EQ(r.index, -1);
}

UTEST(match_path, short_hook_without_configured_continuation_keeps_straight)
{
    Pt p[] = {{0,0},{0,100},{0,200},{0,300},{0,400},{0,500},
              {0,600},{0,700},{0,800},{0,900},{0,1000},
              {20,1000},{40,1000},{60,1000}};
    const char *keys[] = {"2"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 0);
}

UTEST(match_path, short_unconfigured_opposite_turn_keeps_straight)
{
    Pt p[] = {{0,0},{0,100},{0,200},{0,300},{0,400},{0,500},
              {0,600},{0,700},{0,800},{0,900},{0,1000},
              {-20,1000},{-40,1000},{-60,1000}};
    const char *keys[] = {"2", "26"}; /* 只配置向右延伸，实际短尾巴向左。 */
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 0);
}

UTEST(match_path, boundary_direction_is_rejected_as_ambiguous)
{
    /* 约 22.5°，恰在右(6)与右下(3)之间。旧量化只能武断选一边。 */
    Pt p[] = {{0,0},{20,8},{40,17},{60,25},{80,33},{100,41}};
    const char *keys[] = {"6", "3"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), -1);
    ASSERT_TRUE(r.score >= 82);
    ASSERT_TRUE(r.score - r.second_score < 6);
}

UTEST(match_path, clear_nearest_direction_is_accepted)
{
    Pt p[] = {{0,0},{25,7},{50,13},{75,20},{100,27}}; /* 约 15°，明确偏右 */
    const char *keys[] = {"6", "3"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 0);
}

UTEST(match_path, low_score_is_rejected_even_with_one_template)
{
    Pt p[] = {{0,0},{-25,0},{-50,0},{-75,0},{-100,0}};
    const char *keys[] = {"6"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), -1);
    ASSERT_TRUE(r.score < 82);
}

UTEST(match_path, wide_steep_v_prefers_visual_topology)
{
    Pt p[256];
    size_t n = make_v(p, NELEMS(p), 75, 200, 2);
    const char *keys[] = {"28", "39"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, n, 20, keys, NELEMS(keys), 82, 6, &r), 1);
    ASSERT_EQ(r.index, 1);
}

UTEST(match_path, narrow_down_up_remains_28)
{
    Pt p[] = {{0,0},{2,20},{4,40},{6,60},{8,80},{10,100},
              {12,80},{14,60},{16,40},{18,20},{20,0}};
    const char *keys[] = {"28", "39"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 0);
}

UTEST(match_path, topology_bonus_is_continuous_across_old_pixel_boundary)
{
    Pt below[] = {{0,0},{20,100},{39,0}};
    Pt above[] = {{0,0},{21,100},{41,0}};
    const char *keys[] = {"39"};
    RecMatchResult a, b;
    ASSERT_EQ(rec_match_path(below, NELEMS(below), 20, keys, 1, 0, 0, &a), 0);
    ASSERT_EQ(rec_match_path(above, NELEMS(above), 20, keys, 1, 0, 0, &b), 0);
    int difference = a.score > b.score ? a.score - b.score : b.score - a.score;
    ASSERT_TRUE(difference <= 3); /* 旧硬阈值会在这里跳变约 18 分 */
}

UTEST(match_path, topology_score_changes_smoothly_over_full_boundary_band)
{
    const char *keys[] = {"39"};
    int previous = -1;
    for (int width = 24; width <= 56; width++) {
        Pt p[] = {{0,0},{width / 2,100},{width,0}};
        RecMatchResult r;
        ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, 1, 0, 0, &r), 0);
        ASSERT_TRUE(r.score >= 0 && r.score <= 100);
        if (previous >= 0) {
            int difference = r.score > previous ? r.score - previous : previous - r.score;
            ASSERT_TRUE(difference <= 3);
        }
        previous = r.score;
    }
}

UTEST(match_path, no_effective_segment_never_matches)
{
    Pt p[] = {{0,0},{4,2},{7,-2},{9,1}};
    const char *keys[] = {"6", "2", "26"};
    RecMatchResult r;
    ASSERT_FALSE(rec_has_gesture(p, NELEMS(p), 20));
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 1, 0, &r), -1);
    ASSERT_EQ(r.index, -1);
}

UTEST(match_path, invalid_templates_are_ignored)
{
    Pt p[] = {{0,0},{25,0},{50,0},{75,0},{100,0}};
    const char *keys[] = {"hello", "5", "6"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 2);
}

UTEST(match_path, equivalent_templates_do_not_create_false_ambiguity)
{
    Pt p[] = {{0,0},{25,0},{50,0},{75,0},{100,0}};
    const char *keys[] = {"6", "66"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 0);
    ASSERT_EQ(r.score, 100);
    ASSERT_EQ(r.second_score, 0);
}

UTEST(match_path, randomized_cardinal_and_diagonal_matrix)
{
    const char *keys[] = {"1", "2", "3", "4", "6", "7", "8", "9"};
    Pt p[256];
    char msg[96];
    for (int expected = 0; expected < 8; expected++) {
        for (unsigned variant = 1; variant <= 20; variant++) {
            size_t n = make_key_path(p, NELEMS(p), keys[expected],
                                     150, 14, 3, variant * 97u + (unsigned)expected);
            RecMatchResult r;
            int got = rec_match_path(p, n, 20, keys, NELEMS(keys), 82, 6, &r);
            snprintf(msg, sizeof(msg), "方向%s 随机变体%u score=%d second=%d",
                     keys[expected], variant, r.score, r.second_score);
            ASSERT_EQ_MSG(got, expected, msg);
        }
    }
}

UTEST(match_path, randomized_multisegment_matrix)
{
    const char *keys[] = {"1", "2", "3", "4", "6", "7", "8", "9",
                          "26", "39", "86"};
    const char *patterns[] = {"26", "39", "86"};
    const int expected[] = {8, 9, 10};
    Pt p[256];
    char msg[112];
    for (int k = 0; k < 3; k++) {
        for (unsigned variant = 1; variant <= 24; variant++) {
            size_t n = make_key_path(p, NELEMS(p), patterns[k],
                                     170, 12, 3, variant * 193u + (unsigned)k);
            RecMatchResult r;
            int got = rec_match_path(p, n, 20, keys, NELEMS(keys), 82, 6, &r);
            snprintf(msg, sizeof(msg), "手势%s 随机变体%u score=%d second=%d",
                     patterns[k], variant, r.score, r.second_score);
            ASSERT_EQ_MSG(got, expected[k], msg);
        }
    }
}

UTEST(match_path, maximum_input_and_candidate_load_is_deterministic)
{
    enum { POINT_COUNT = 4096, CANDIDATE_COUNT = 192 };
    static Pt p[POINT_COUNT];
    static char storage[CANDIDATE_COUNT][16];
    const char *keys[CANDIDATE_COUNT];
    const char digits[] = "12346789";

    for (int i = 0; i < POINT_COUNT; i++) {
        p[i].x = i * 2;
        p[i].y = (i % 7 == 0) ? 1 : 0;
    }
    snprintf(storage[0], sizeof(storage[0]), "6");
    keys[0] = storage[0];
    for (int i = 1; i < CANDIDATE_COUNT; i++) {
        unsigned value = (unsigned)(i - 1);
        char previous = '\0';
        for (int j = 0; j < 7; j++) {
            unsigned radix = j == 0 ? 8u : 7u;
            unsigned choice = value % radix;
            value /= radix;
            char digit = '\0';
            for (int d = 0; d < 8; d++) {
                if (digits[d] == previous)
                    continue;
                if (choice == 0u) {
                    digit = digits[d];
                    break;
                }
                choice--;
            }
            storage[i][j] = digit;
            previous = digit;
        }
        storage[i][7] = '\0';
        keys[i] = storage[i];
    }

    RecMatchResult first, second;
    ASSERT_EQ(rec_match_path(p, POINT_COUNT, 20, keys, CANDIDATE_COUNT, 82, 6, &first), 0);
    ASSERT_EQ(rec_match_path(p, POINT_COUNT, 20, keys, CANDIDATE_COUNT, 82, 6, &second), 0);
    ASSERT_EQ(first.index, second.index);
    ASSERT_EQ(first.score, second.score);
    ASSERT_EQ(first.second_score, second.second_score);
    ASSERT_TRUE(first.score >= 0 && first.score <= 100);
    ASSERT_TRUE(first.second_score >= 0 && first.second_score <= 100);
}

UTEST(match_path, deterministic_paths_are_translation_invariant)
{
    const char *keys[] = {"1", "2", "3", "4", "6", "7", "8", "9",
                          "26", "39", "86"};
    const char *patterns[] = {"1", "6", "9", "26", "39", "86"};
    Pt original[256], translated[256];

    for (size_t k = 0; k < NELEMS(patterns); k++) {
        for (unsigned variant = 1; variant <= 12; variant++) {
            size_t n = make_key_path(original, NELEMS(original), patterns[k],
                                     160, 12, 3, variant * 911u + (unsigned)k);
            for (size_t i = 0; i < n; i++) {
                translated[i].x = original[i].x + 10000;
                translated[i].y = original[i].y - 7000;
            }
            RecMatchResult a, b;
            int ia = rec_match_path(original, n, 20, keys, NELEMS(keys), 82, 6, &a);
            int ib = rec_match_path(translated, n, 20, keys, NELEMS(keys), 82, 6, &b);
            ASSERT_EQ(ia, ib);
            ASSERT_EQ(a.score, b.score);
            ASSERT_EQ(a.second_score, b.second_score);
        }
    }
}

UTEST(match_path, extreme_integer_coordinates_remain_bounded)
{
    Pt p[] = {{INT_MIN,INT_MIN},{0,0},{INT_MAX,INT_MAX}};
    const char *keys[] = {"3", "7"};
    RecMatchResult r;
    ASSERT_EQ(rec_match_path(p, NELEMS(p), 20, keys, NELEMS(keys), 82, 6, &r), 0);
    ASSERT_TRUE(r.score >= 0 && r.score <= 100);
    ASSERT_TRUE(r.second_score >= 0 && r.second_score <= 100);
}

UTEST_MAIN();
