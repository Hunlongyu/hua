/*
 * recognizer.c —— 见 recognizer.h。
 */
#include "recognizer.h"

#include <math.h>
#include <string.h>

/* 位移向量 → 九宫格方向数字（8 扇区，每扇 45°；屏幕坐标 y 向下）。 */
static char quantize(int dx, int dy)
{
    const double PI = 3.14159265358979323846;
    double ang = atan2((double)dy, (double)dx);   /* (-PI, PI] */
    if (ang < 0)
        ang += 2.0 * PI;
    /* 就近取 8 扇区：0=右 1=右下 2=下 3=左下 4=左 5=左上 6=上 7=右上 */
    int sector = (int)floor(ang / (PI / 4.0) + 0.5) % 8;
    static const char table[8] = {'6', '3', '2', '1', '4', '7', '8', '9'};
    return table[sector];
}

/* 对角方向 d 的两个正交分量（如 3=右下 由 2下 与 6右 组成）；非对角返回 0。 */
static void diag_components(char d, char *a, char *b)
{
    switch (d) {
    case '1': *a = '2'; *b = '4'; return;   /* 左下 = 下 + 左 */
    case '3': *a = '2'; *b = '6'; return;   /* 右下 = 下 + 右 */
    case '7': *a = '8'; *b = '4'; return;   /* 左上 = 上 + 左 */
    case '9': *a = '8'; *b = '6'; return;   /* 右上 = 上 + 右 */
    default:  *a = 0;   *b = 0;   return;
    }
}

#define REC_MAX_SEG 128

size_t rec_encode(const Pt *pts, size_t n, int min_dist,
                  char *out, size_t out_cap)
{
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    if (!pts || n < 2 || min_dist <= 0)
        return 0;

    /* 64 位：long 在 Windows（LLP64）恒为 32 位，坐标差的平方和可能溢出（UB）。 */
    const long long min2 = (long long)min_dist * (long long)min_dist;

    /* 第一步：切成方向段，记录每段累计路径长度。 */
    char   sdir[REC_MAX_SEG];
    double slen[REC_MAX_SEG];
    int    sc = 0;
    Pt     anchor = pts[0];

    for (size_t i = 1; i < n; i++) {
        int dx = pts[i].x - anchor.x;
        int dy = pts[i].y - anchor.y;
        long long d2 = (long long)dx * dx + (long long)dy * dy;
        if (d2 < min2)
            continue;     /* 距段起点位移不足阈值：抖动，忽略 */

        char d = quantize(dx, dy);
        double l = sqrt((double)d2);
        if (sc == 0 || sdir[sc - 1] != d) {
            if (sc < REC_MAX_SEG) {
                sdir[sc] = d;
                slen[sc] = l;
                sc++;
            }
        } else {
            slen[sc - 1] += l;   /* 合并连续同向，累加长度 */
        }
        anchor = pts[i];
    }

    /* 第二步：删除"拐角伪影"——夹在两个正交分量之间、且较短的对角段。
       例如 下(2) → 右下(3, 短) → 右(6)：那段 3 是拐角噪声，删之得 2,6。
       长对角（真正的对角手势）不会被删。 */
    char keep[REC_MAX_SEG];
    for (int k = 0; k < sc; k++)
        keep[k] = 1;
    const double bridge_max = (double)min_dist * 1.6;
    for (int k = 1; k + 1 < sc; k++) {
        char ca, cb;
        diag_components(sdir[k], &ca, &cb);
        if (!ca || slen[k] >= bridge_max)
            continue;
        char a = sdir[k - 1], b = sdir[k + 1];
        if ((a == ca && b == cb) || (a == cb && b == ca))
            keep[k] = 0;
    }

    /* 第三步：输出（保险起见再次合并相邻同向）。 */
    size_t len = 0;
    char   last = 0;
    for (int k = 0; k < sc; k++) {
        if (!keep[k] || sdir[k] == last)
            continue;
        if (len + 1 < out_cap) {
            out[len++] = sdir[k];
            out[len] = '\0';
            last = sdir[k];
        }
    }
    return len;
}

int rec_levenshtein(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    if (la == 0) return (int)lb;
    if (lb == 0) return (int)la;

    /*
     * 定长栈缓冲：两个输入都受 REC_MAX_SEQ 约束（a 是 rec_encode 的产物，
     * b 是手势模板，更短），无需堆分配。此前这里每次调用都 malloc 两次，而
     * rec_match 会在手势进行中逐模板调用、每帧 60fps 跑一轮——默认约 10 个手势
     * 即上千次/秒的分配，且分配失败会被当成「不匹配」静默吞掉。
     */
    if (la > REC_MAX_SEQ || lb > REC_MAX_SEQ)
        return -1;   /* 超出约定容量：按不匹配处理（调用方已判 d < 0） */

    int buf_a[REC_MAX_SEQ + 1], buf_b[REC_MAX_SEQ + 1];
    int *prev = buf_a, *cur = buf_b;

    for (size_t j = 0; j <= lb; j++)
        prev[j] = (int)j;

    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int m = del < ins ? del : ins;
            if (sub < m) m = sub;
            cur[j] = m;
        }
        int *t = prev; prev = cur; cur = t;
    }

    return prev[lb];
}

int rec_match(const char *seq, const char *const *keys,
              size_t key_count, int tolerance)
{
    if (!seq || !keys)
        return -1;

    int    best = -1;
    int    best_dist = 0;
    size_t best_len = 0;

    for (size_t i = 0; i < key_count; i++) {
        if (!keys[i])
            continue;
        int d = rec_levenshtein(seq, keys[i]);
        if (d < 0 || d > tolerance)
            continue;
        size_t klen = strlen(keys[i]);
        /* 更小距离胜；平票时更短模板胜；仍平则保留更靠前者（严格 < 才替换）。 */
        if (best < 0 || d < best_dist || (d == best_dist && klen < best_len)) {
            best = (int)i;
            best_dist = d;
            best_len = klen;
        }
    }
    return best;
}
