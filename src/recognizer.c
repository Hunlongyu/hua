/*
 * recognizer.c —— 小体积、无训练的几何轨迹识别。
 *
 * 算法流水线：
 *   原始点 → 等距形状采样 + RDP → 连续向量段 → 自适应转向迟滞
 *          → 方向/DTW/转角/形状联合评分 → 阈值与第一、二名间距拒识
 *
 * 实现只使用定长栈数组，不分配堆内存，也不依赖神经网络或第三方库。
 */
#include "recognizer.h"

#include <math.h>
#include <string.h>

#define REC_INPUT_POINTS  256
#define REC_RAW_SEGMENTS   128
#define REC_MAX_SEGMENTS    32
#define REC_RESAMPLE_POINTS 32

#define REC_PI       3.14159265358979323846
#define REC_TWO_PI   (2.0 * REC_PI)
#define REC_TURN_MERGE (REC_PI / 8.0)  /* 22.5°：小于此角度仍视为同一笔 */

typedef struct {
    double x, y;
} DPoint;

typedef struct {
    double dx, dy;
    double length;          /* 累计路径长；方向由净向量 dx/dy 决定 */
    double angle;
    double weight;
} Segment;

typedef struct {
    Segment seg[REC_MAX_SEGMENTS];
    int count;
    double total_length;
    DPoint shape[REC_RESAMPLE_POINTS];
    char topology[3];       /* 明显 V/尖括号形的视觉方向对；空串表示无特殊拓扑 */
    double topology_confidence;
    Segment provisional_tail; /* 达到 MinDistance、但尚未通过自适应确认的末段 */
    bool has_provisional_tail;
} Pattern;

typedef struct {
    double angle[REC_MAX_SEQ];
    double ux[REC_MAX_SEQ], uy[REC_MAX_SEQ];
    int count;
    DPoint shape[REC_RESAMPLE_POINTS];
    char key[REC_MAX_SEQ + 1];
} Template;

typedef struct {
    unsigned short lo, hi;
} Range;

static double min_d(double a, double b) { return a < b ? a : b; }
static double max_d(double a, double b) { return a > b ? a : b; }

static double clamp_d(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static double ramp(double v, double lo, double hi)
{
    if (hi <= lo)
        return v >= hi ? 1.0 : 0.0;
    return clamp_d((v - lo) / (hi - lo), 0.0, 1.0);
}

static double vector_length(double dx, double dy)
{
    return sqrt(dx * dx + dy * dy);
}

/* 两个绝对方向的最小夹角，范围 0..PI。 */
static double angle_distance(double a, double b)
{
    double d = fabs(a - b);
    while (d > REC_TWO_PI)
        d -= REC_TWO_PI;
    return d > REC_PI ? REC_TWO_PI - d : d;
}

/* 从 a 转到 b 的有符号最短转角，范围 (-PI, PI]。 */
static double signed_turn(double a, double b)
{
    double d = b - a;
    while (d <= -REC_PI)
        d += REC_TWO_PI;
    while (d > REC_PI)
        d -= REC_TWO_PI;
    return d;
}

static bool is_direction_digit(char c)
{
    return c == '1' || c == '2' || c == '3' || c == '4' ||
           c == '6' || c == '7' || c == '8' || c == '9';
}

size_t rec_normalize_template(const char *key, char *out, size_t out_cap)
{
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    if (!key || !key[0])
        return 0;

    size_t length = 0;
    char previous = '\0';
    for (size_t i = 0; key[i]; i++) {
        if (!is_direction_digit(key[i])) {
            out[0] = '\0';
            return 0;
        }
        if (key[i] == previous)
            continue;
        if (length + 1 >= out_cap) {
            out[0] = '\0';
            return 0;
        }
        out[length++] = key[i];
        previous = key[i];
    }
    out[length] = '\0';
    return length;
}

/*
 * 点太多时按原序均匀抽取到固定上限。hook 已按 StepDistance 采样，故按下标抽取
 * 近似按弧长抽取；这里的目的只是给 RDP 一个确定的最坏开销，而非再次平滑轨迹。
 */
static size_t reduce_input(const Pt *pts, size_t n, DPoint *out, size_t cap)
{
    if (!pts || !out || n == 0 || cap == 0)
        return 0;

    size_t wanted = n < cap ? n : cap;
    size_t count = 0;
    for (size_t j = 0; j < wanted; j++) {
        size_t i = wanted == 1 ? 0 : j * (n - 1) / (wanted - 1);
        DPoint p = {(double)pts[i].x, (double)pts[i].y};
        if (count && out[count - 1].x == p.x && out[count - 1].y == p.y)
            continue;
        out[count++] = p;
    }
    return count;
}

static double point_segment_distance2(DPoint p, DPoint a, DPoint b)
{
    double vx = b.x - a.x, vy = b.y - a.y;
    double wx = p.x - a.x, wy = p.y - a.y;
    double vv = vx * vx + vy * vy;
    if (vv <= 0.0)
        return wx * wx + wy * wy;
    double t = clamp_d((wx * vx + wy * vy) / vv, 0.0, 1.0);
    double dx = p.x - (a.x + t * vx);
    double dy = p.y - (a.y + t * vy);
    return dx * dx + dy * dy;
}

/* 非递归 Ramer-Douglas-Peucker，避免轨迹恶意锯齿导致深递归。 */
static size_t simplify_rdp(const DPoint *in, size_t n, double epsilon,
                           DPoint *out, size_t out_cap)
{
    if (!in || !out || n == 0 || out_cap == 0)
        return 0;
    if (n <= 2) {
        size_t count = n < out_cap ? n : out_cap;
        for (size_t i = 0; i < count; i++)
            out[i] = in[i];
        return count;
    }

    unsigned char keep[REC_INPUT_POINTS] = {0};
    Range stack[REC_INPUT_POINTS * 2];
    size_t top = 0;
    double eps2 = epsilon * epsilon;
    keep[0] = keep[n - 1] = 1;
    stack[top++] = (Range){0, (unsigned short)(n - 1)};

    while (top) {
        Range r = stack[--top];
        if ((size_t)r.hi <= (size_t)r.lo + 1)
            continue;

        double farthest = -1.0;
        unsigned short split = r.lo;
        for (unsigned short i = (unsigned short)(r.lo + 1); i < r.hi; i++) {
            double d2 = point_segment_distance2(in[i], in[r.lo], in[r.hi]);
            if (d2 > farthest) {
                farthest = d2;
                split = i;
            }
        }
        if (farthest > eps2 && split > r.lo && split < r.hi) {
            keep[split] = 1;
            stack[top++] = (Range){r.lo, split};
            stack[top++] = (Range){split, r.hi};
        }
    }

    size_t count = 0;
    for (size_t i = 0; i < n && count < out_cap; i++) {
        if (keep[i])
            out[count++] = in[i];
    }
    return count;
}

/* 将原轨迹按弧长重采样到固定点数，并以起点平移、总弧长缩放。 */
static double resample_shape(const DPoint *pts, size_t n,
                             DPoint out[REC_RESAMPLE_POINTS])
{
    double cumulative[REC_INPUT_POINTS];
    cumulative[0] = 0.0;
    for (size_t i = 1; i < n; i++) {
        double dx = pts[i].x - pts[i - 1].x;
        double dy = pts[i].y - pts[i - 1].y;
        cumulative[i] = cumulative[i - 1] + vector_length(dx, dy);
    }
    double total = cumulative[n - 1];
    if (total <= 0.0) {
        memset(out, 0, sizeof(DPoint) * REC_RESAMPLE_POINTS);
        return 0.0;
    }

    size_t edge = 1;
    for (int k = 0; k < REC_RESAMPLE_POINTS; k++) {
        double target = total * (double)k / (double)(REC_RESAMPLE_POINTS - 1);
        while (edge + 1 < n && cumulative[edge] < target)
            edge++;
        double start_d = cumulative[edge - 1];
        double span = cumulative[edge] - start_d;
        double t = span > 0.0 ? (target - start_d) / span : 0.0;
        double x = pts[edge - 1].x + (pts[edge].x - pts[edge - 1].x) * t;
        double y = pts[edge - 1].y + (pts[edge].y - pts[edge - 1].y) * t;
        out[k].x = (x - pts[0].x) / total;
        out[k].y = (y - pts[0].y) / total;
    }
    return total;
}

static void merge_segment(Segment *dst, double dx, double dy, double path_length)
{
    dst->dx += dx;
    dst->dy += dy;
    dst->length += path_length;
    if (dst->dx != 0.0 || dst->dy != 0.0)
        dst->angle = atan2(dst->dy, dst->dx);
}

static char quadrant_digit(double dx, double dy)
{
    if (dx > 0.0)
        return dy > 0.0 ? '3' : '9';
    return dy > 0.0 ? '1' : '7';
}

/*
 * 两段明显向同一开口轴展开、在另一轴反向分叉时，保留“视觉 V”语义。
 * 这解决了陡 V 的两臂落入竖直扇区、与近似原路折返混淆的问题。规则只产生一个
 * 拓扑提示；最终仍要通过连续几何最低分，不会绕开拒识。
 */
static void detect_two_segment_topology(Pattern *p, int min_dist)
{
    p->topology[0] = '\0';
    p->topology_confidence = 0.0;
    if (p->count != 2)
        return;

    Segment *a = &p->seg[0], *b = &p->seg[1];
    double clear = (double)min_dist;
    int sx1 = a->dx > 0.0 ? 1 : (a->dx < 0.0 ? -1 : 0);
    int sx2 = b->dx > 0.0 ? 1 : (b->dx < 0.0 ? -1 : 0);
    int sy1 = a->dy > 0.0 ? 1 : (a->dy < 0.0 ? -1 : 0);
    int sy2 = b->dy > 0.0 ? 1 : (b->dy < 0.0 ? -1 : 0);
    int open_x = sx1 != 0 && sx1 == sx2 && sy1 != 0 && sy1 == -sy2;
    int open_y = sy1 != 0 && sy1 == sy2 && sx1 != 0 && sx1 == -sx2;
    if (!open_x && !open_y)
        return;

    double width, height, shorter, min_component;
    if (open_x) {
        width = fabs(a->dx + b->dx);
        height = max_d(fabs(a->dy), fabs(b->dy));
        shorter = min_d(fabs(a->dy), fabs(b->dy));
        min_component = min_d(min_d(fabs(a->dx), fabs(b->dx)), shorter);
    } else {
        width = fabs(a->dy + b->dy);
        height = max_d(fabs(a->dx), fabs(b->dx));
        shorter = min_d(fabs(a->dx), fabs(b->dx));
        min_component = min_d(min_d(fabs(a->dy), fabs(b->dy)), shorter);
    }

    if (height <= 0.0)
        return;

    /*
     * 旧实现跨过任一硬边界就一次性 +18 分，1px 波动足以改变动作。现在每项都在
     * 原阈值两侧平滑过渡，最终取最弱证据：形状越接近明确开口，置信度才越高。
     */
    double confidence = ramp(min_component, clear * 0.50, clear * 1.50);
    confidence = min_d(confidence, ramp(width, clear, clear * 3.0));
    confidence = min_d(confidence, ramp(height, clear, clear * 3.0));
    confidence = min_d(confidence, ramp(width / height, 0.15, 0.45));
    confidence = min_d(confidence, ramp(shorter / height, 0.25, 0.55));
    if (confidence <= 0.0)
        return;

    p->topology[0] = quadrant_digit(a->dx, a->dy);
    p->topology[1] = quadrant_digit(b->dx, b->dy);
    p->topology[2] = '\0';
    p->topology_confidence = confidence;
}

static bool finalize_pattern(Pattern *p, int min_dist)
{
    p->total_length = 0.0;
    for (int i = 0; i < p->count; i++)
        p->total_length += p->seg[i].length;
    if (p->total_length <= 0.0)
        return false;
    for (int i = 0; i < p->count; i++)
        p->seg[i].weight = p->seg[i].length / p->total_length;
    detect_two_segment_topology(p, min_dist);
    return true;
}

static bool build_pattern(const Pt *pts, size_t n, int min_dist, Pattern *p)
{
    memset(p, 0, sizeof(*p));
    if (!pts || n < 2 || min_dist <= 0)
        return false;

    DPoint input[REC_INPUT_POINTS];
    DPoint simplified[REC_INPUT_POINTS];
    size_t input_count = reduce_input(pts, n, input, REC_INPUT_POINTS);
    if (input_count < 2)
        return false;

    double raw_total = resample_shape(input, input_count, p->shape);
    if (raw_total < (double)min_dist)
        return false;

    /* 绝对像素 epsilon 由用户的 MinDistance 派生；既过滤手抖，也不随手势尺寸膨胀。 */
    double epsilon = max_d(2.0, (double)min_dist * 0.42);
    size_t simple_count = simplify_rdp(input, input_count, epsilon,
                                       simplified, REC_INPUT_POINTS);
    if (simple_count < 2)
        return false;

    Segment raw[REC_RAW_SEGMENTS];
    int raw_count = 0;
    for (size_t i = 1; i < simple_count; i++) {
        double dx = simplified[i].x - simplified[i - 1].x;
        double dy = simplified[i].y - simplified[i - 1].y;
        double length = vector_length(dx, dy);
        if (length <= 0.0)
            continue;
        double angle = atan2(dy, dx);

        if (raw_count > 0 && angle_distance(raw[raw_count - 1].angle, angle) < REC_TURN_MERGE) {
            merge_segment(&raw[raw_count - 1], dx, dy, length);
        } else if (raw_count < REC_RAW_SEGMENTS) {
            raw[raw_count].dx = dx;
            raw[raw_count].dy = dy;
            raw[raw_count].length = length;
            raw[raw_count].angle = angle;
            raw[raw_count].weight = 0.0;
            raw_count++;
        } else {
            /* 极端锯齿轨迹超出内部上限时合并尾部，保持确定时间与内存上界。 */
            merge_segment(&raw[raw_count - 1], dx, dy, length);
        }
    }

    for (int i = 0; i < raw_count; i++) {
        if (raw[i].length < (double)min_dist)
            continue;

        if (p->count > 0) {
            Segment *prev = &p->seg[p->count - 1];
            if (angle_distance(prev->angle, raw[i].angle) < REC_TURN_MERGE) {
                merge_segment(prev, raw[i].dx, raw[i].dy, raw[i].length);
                p->has_provisional_tail = false;
                continue;
            }

            /* FlowMouse 的关键经验：前一笔越长，确认一条新方向需要的证据越长。
             * 上限为 6*MinDistance，避免超长第一笔让后续合法转角永远无法成立。 */
            double turn_threshold = (double)min_dist + prev->length * 0.10;
            turn_threshold = min_d(turn_threshold, (double)min_dist * 6.0);
            if (raw[i].length < turn_threshold) {
                /*
                 * 不把它计入稳定分段，但保留为候选感知的“暂定末段”。最终如果稳定
                 * 轨迹要执行前缀，而加上这段后另一个已配置手势成为最佳，就拒识而不
                 * 误执行前缀。若配置中没有相应延伸手势，小钩仍可安全忽略。
                 */
                p->provisional_tail = raw[i];
                p->has_provisional_tail = true;
                continue;
            }
        }

        if (p->count < REC_MAX_SEGMENTS)
            p->seg[p->count++] = raw[i];
        else
            merge_segment(&p->seg[p->count - 1], raw[i].dx, raw[i].dy, raw[i].length);
        p->has_provisional_tail = false;
    }

    /* RDP 在极短、近直线输入上仍可能只留下一个略短于阈值的边；净位移兜底。 */
    if (p->count == 0) {
        double dx = input[input_count - 1].x - input[0].x;
        double dy = input[input_count - 1].y - input[0].y;
        double length = vector_length(dx, dy);
        if (length < (double)min_dist)
            return false;
        p->seg[0] = (Segment){dx, dy, raw_total, atan2(dy, dx), 1.0};
        p->count = 1;
    }

    return finalize_pattern(p, min_dist);
}

static bool digit_geometry(char c, double *angle, double *ux, double *uy)
{
    const double diagonal = 0.70710678118654752440;
    switch (c) {
    case '6': *angle = 0.0;                 *ux = 1.0;       *uy = 0.0;       return true;
    case '3': *angle = REC_PI / 4.0;        *ux = diagonal;  *uy = diagonal;  return true;
    case '2': *angle = REC_PI / 2.0;        *ux = 0.0;       *uy = 1.0;       return true;
    case '1': *angle = 3.0 * REC_PI / 4.0;  *ux = -diagonal; *uy = diagonal;  return true;
    case '4': *angle = REC_PI;              *ux = -1.0;      *uy = 0.0;       return true;
    case '7': *angle = -3.0 * REC_PI / 4.0; *ux = -diagonal; *uy = -diagonal; return true;
    case '8': *angle = -REC_PI / 2.0;       *ux = 0.0;       *uy = -1.0;      return true;
    case '9': *angle = -REC_PI / 4.0;       *ux = diagonal;  *uy = -diagonal; return true;
    default:                                                                return false;
    }
}

static void make_template_shape(Template *t)
{
    for (int k = 0; k < REC_RESAMPLE_POINTS; k++) {
        double distance = (double)t->count * (double)k /
                          (double)(REC_RESAMPLE_POINTS - 1);
        int full = (int)floor(distance);
        double fraction = distance - (double)full;
        if (full >= t->count) {
            full = t->count;
            fraction = 0.0;
        }

        double x = 0.0, y = 0.0;
        for (int i = 0; i < full; i++) {
            x += t->ux[i];
            y += t->uy[i];
        }
        if (full < t->count) {
            x += t->ux[full] * fraction;
            y += t->uy[full] * fraction;
        }
        t->shape[k].x = x / (double)t->count;
        t->shape[k].y = y / (double)t->count;
    }
}

static bool parse_template(const char *key, Template *t)
{
    memset(t, 0, sizeof(*t));
    size_t normalized_length = rec_normalize_template(key, t->key, sizeof(t->key));
    if (normalized_length == 0)
        return false;

    for (size_t i = 0; i < normalized_length; i++) {
        double angle, ux, uy;
        if (!digit_geometry(t->key[i], &angle, &ux, &uy))
            return false; /* rec_normalize_template 已校验；仅保留防御性检查 */
        if (t->count >= REC_MAX_SEQ)
            return false;
        t->angle[t->count] = angle;
        t->ux[t->count] = ux;
        t->uy[t->count] = uy;
        t->count++;
    }
    if (t->count == 0)
        return false;
    make_template_shape(t);
    return true;
}

static char angle_digit(double angle)
{
    static const char sector_char[8] = {'6', '3', '2', '1', '4', '7', '8', '9'};
    while (angle < 0.0)
        angle += REC_TWO_PI;
    while (angle >= REC_TWO_PI)
        angle -= REC_TWO_PI;
    int sector = (int)floor(angle / (REC_PI / 4.0) + 0.5) & 7;
    return sector_char[sector];
}

size_t rec_encode(const Pt *pts, size_t n, int min_dist,
                  char *out, size_t out_cap)
{
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';

    Pattern p;
    if (!build_pattern(pts, n, min_dist, &p))
        return 0;

    if (p.topology[0] && p.topology_confidence >= 0.50) {
        size_t need = strlen(p.topology);
        size_t copy = need < out_cap - 1 ? need : out_cap - 1;
        memcpy(out, p.topology, copy);
        out[copy] = '\0';
        return copy;
    }

    size_t length = 0;
    char previous = '\0';
    for (int i = 0; i < p.count; i++) {
        char c = angle_digit(p.seg[i].angle);
        if (c == previous)
            continue;
        if (length + 1 >= out_cap)
            break;
        out[length++] = c;
        out[length] = '\0';
        previous = c;
    }
    return length;
}

bool rec_has_gesture(const Pt *pts, size_t n, int min_dist)
{
    Pattern p;
    return build_pattern(pts, n, min_dist, &p);
}

/* 两个方向序列的 DTW，局部代价为角差/PI。 */
static double direction_dtw(const double *a, int na, const double *b, int nb)
{
    double prev[REC_MAX_SEQ + 1], cur[REC_MAX_SEQ + 1];
    int prev_steps[REC_MAX_SEQ + 1], cur_steps[REC_MAX_SEQ + 1];
    const double inf = 1.0e30;

    for (int j = 0; j <= nb; j++) {
        prev[j] = inf;
        prev_steps[j] = 0;
    }
    prev[0] = 0.0;

    for (int i = 1; i <= na; i++) {
        cur[0] = inf;
        cur_steps[0] = 0;
        for (int j = 1; j <= nb; j++) {
            double best = prev[j - 1];
            int steps = prev_steps[j - 1];
            if (prev[j] < best) {
                best = prev[j];
                steps = prev_steps[j];
            }
            if (cur[j - 1] < best) {
                best = cur[j - 1];
                steps = cur_steps[j - 1];
            }
            cur[j] = best + angle_distance(a[i - 1], b[j - 1]) / REC_PI;
            cur_steps[j] = steps + 1;
        }
        for (int j = 0; j <= nb; j++) {
            prev[j] = cur[j];
            prev_steps[j] = cur_steps[j];
        }
    }
    return prev_steps[nb] > 0 ? prev[nb] / (double)prev_steps[nb] : 1.0;
}

/* 按双方线段在总路径中的占比对齐，等价于在弧长轴上比较方向函数。 */
static double proportion_cost(const Pattern *p, const Template *t)
{
    int i = 0, j = 0;
    double remain_a = p->seg[0].weight;
    double remain_b = 1.0 / (double)t->count;
    double cost = 0.0;

    while (i < p->count && j < t->count) {
        double overlap = min_d(remain_a, remain_b);
        cost += overlap * angle_distance(p->seg[i].angle, t->angle[j]) / REC_PI;
        remain_a -= overlap;
        remain_b -= overlap;
        if (remain_a <= 1.0e-9 && ++i < p->count)
            remain_a = p->seg[i].weight;
        if (remain_b <= 1.0e-9 && ++j < t->count)
            remain_b = 1.0 / (double)t->count;
    }
    return clamp_d(cost, 0.0, 1.0);
}

static double turn_cost(const Pattern *p, const Template *t)
{
    int pa = p->count - 1;
    int tb = t->count - 1;
    if (pa == 0 && tb == 0)
        return 0.0;
    if (pa == 0 || tb == 0)
        return 0.75;

    double a[REC_MAX_SEGMENTS - 1];
    double b[REC_MAX_SEQ - 1];
    for (int i = 0; i < pa; i++)
        a[i] = signed_turn(p->seg[i].angle, p->seg[i + 1].angle);
    for (int i = 0; i < tb; i++)
        b[i] = signed_turn(t->angle[i], t->angle[i + 1]);
    return direction_dtw(a, pa, b, tb);
}

static double shape_cost(const Pattern *p, const Template *t)
{
    double sum = 0.0;
    for (int i = 0; i < REC_RESAMPLE_POINTS; i++) {
        double dx = p->shape[i].x - t->shape[i].x;
        double dy = p->shape[i].y - t->shape[i].y;
        sum += vector_length(dx, dy);
    }
    return clamp_d(sum / (double)REC_RESAMPLE_POINTS, 0.0, 1.0);
}

static int template_score(const Pattern *p, const Template *t)
{
    double observed_angles[REC_MAX_SEGMENTS];
    for (int i = 0; i < p->count; i++)
        observed_angles[i] = p->seg[i].angle;

    double proportion = proportion_cost(p, t);
    double direction = direction_dtw(observed_angles, p->count, t->angle, t->count);
    double turns = turn_cost(p, t);
    double shape = shape_cost(p, t);
    double count = fabs((double)p->count - (double)t->count) /
                   max_d((double)p->count, (double)t->count);

    /* 方向是主信息；转角与形状负责区分相同方向集合但顺序/结构不同的轨迹。 */
    double cost = 0.42 * proportion + 0.18 * direction +
                  0.20 * turns + 0.15 * shape + 0.05 * count;
    double score = (1.0 - clamp_d(cost, 0.0, 1.0)) * 100.0;

    /* 陡 V 的中心角更接近“下、上”，但其明确横向开口拓扑应优先于扇区中心距离。
     * 只对通过宽度/深度双重判据的两段形状加分，普通边界线不会获得该加成。 */
    if (p->topology[0] && strcmp(p->topology, t->key) == 0)
        score = min_d(100.0, score + 18.0 * p->topology_confidence);

    return (int)floor(score + 0.5);
}

typedef struct {
    int best_index;
    int best;
    int second;
} Ranking;

static bool template_seen_before(const char *const *keys, size_t index,
                                 const char *normalized)
{
    for (size_t i = 0; i < index; i++) {
        Template earlier;
        if (parse_template(keys[i], &earlier) && strcmp(earlier.key, normalized) == 0)
            return true;
    }
    return false;
}

static Ranking rank_pattern(const Pattern *p, const char *const *keys, size_t key_count)
{
    Ranking r = {-1, -1, -1};
    for (size_t i = 0; i < key_count; i++) {
        Template t;
        if (!parse_template(keys[i], &t) || template_seen_before(keys, i, t.key))
            continue;
        int score = template_score(p, &t);
        if (score > r.best) {
            r.second = r.best;
            r.best = score;
            r.best_index = (int)i;
        } else if (score > r.second) {
            r.second = score;
        }
    }
    return r;
}

static bool make_provisional_pattern(const Pattern *stable, int min_dist, Pattern *out)
{
    if (!stable->has_provisional_tail || stable->count >= REC_MAX_SEGMENTS)
        return false;
    *out = *stable;
    out->seg[out->count++] = stable->provisional_tail;
    out->has_provisional_tail = false;
    return finalize_pattern(out, min_dist);
}

int rec_match_path(const Pt *pts, size_t n, int min_dist,
                   const char *const *keys, size_t key_count,
                   int min_score, int ambiguity_margin,
                   RecMatchResult *result)
{
    RecMatchResult local = {-1, 0, 0};
    if (!result)
        result = &local;
    *result = local;
    if (!keys || key_count == 0)
        return -1;

    Pattern p;
    if (!build_pattern(pts, n, min_dist, &p))
        return -1;

    min_score = min_score < 0 ? 0 : (min_score > 100 ? 100 : min_score);
    ambiguity_margin = ambiguity_margin < 0 ? 0 :
                       (ambiguity_margin > 100 ? 100 : ambiguity_margin);

    Ranking stable = rank_pattern(&p, keys, key_count);

    if (stable.best_index < 0)
        return -1;
    result->score = stable.best;
    result->second_score = stable.second < 0 ? 0 : stable.second;

    if (stable.best < min_score)
        return -1;
    if (stable.second >= 0 && stable.best - stable.second < ambiguity_margin)
        return -1;

    /*
     * 暂定末段不够长，不能直接把延伸手势判为成功；但也不能删掉它后执行短前缀。
     * 仅当它足以让另一个已配置模板达到最低分并成为最佳时拒识。这样 `2`/`26`
     * 共存时不会把未完成的 `26` 当成 `2`，而只配置 `2` 时仍可忽略末端小钩。
     */
    Pattern provisional;
    if (make_provisional_pattern(&p, min_dist, &provisional)) {
        Ranking extended = rank_pattern(&provisional, keys, key_count);
        if (extended.best_index >= 0 && extended.best_index != stable.best_index &&
            extended.best >= min_score)
            return -1;
    }

    result->index = stable.best_index;
    return stable.best_index;
}
