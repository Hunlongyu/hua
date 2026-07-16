/*
 * recognizer.h —— 方向识别，纯逻辑、无 Win32 依赖，便于单测。
 *
 * 采样点序列 → 8 方向九宫格数字串 → 与手势表精确/容错匹配。
 * 九宫格方向数字：
 *     8=上  2=下  4=左  6=右
 *     7=左上 9=右上 1=左下 3=右下
 * 注意屏幕坐标 y 向下增长（下 = dy>0）。
 */
#ifndef HUA_RECOGNIZER_H
#define HUA_RECOGNIZER_H

#include <stddef.h>

typedef struct {
    int x, y;
} Pt;

/*
 * 方向串缓冲的约定容量（含结尾 '\0'）。rec_encode 的输出契约，调用方的缓冲
 * 应按此声明——若各处自行硬编码且不慎写小了，rec_encode 会静默截断方向串，
 * 表现为手势莫名匹配不上，且无任何报错或日志。
 */
#define REC_MAX_SEQ 64

/*
 * 点序列 → 方向串。
 * - min_dist：单方向被采纳/转折确认的最小累计位移（像素）。低于阈值的抖动被抹掉。
 * - 相邻同向自动合并；输出如 "6"、"26"、"2141"。
 * - out 由调用方提供，写入以 '\0' 结尾的串。
 * 返回写入的方向字符个数（不含结尾 '\0'）。out_cap 不足时按容量截断。
 */
size_t rec_encode(const Pt *pts, size_t n, int min_dist,
                  char *out, size_t out_cap);

/* 标准 Levenshtein 编辑距离。
 * 输入长度须 <= REC_MAX_SEQ（方向串与手势模板均满足）；超长返回 -1。 */
int rec_levenshtein(const char *a, const char *b);

/*
 * 在手势表 keys[0..key_count) 中匹配 seq。
 * - 取编辑距离最小且 <= tolerance 者；tolerance=0 即精确匹配。
 * - 平票时优先更短模板，再优先更靠前者。
 * 返回命中下标，或 -1 表示无匹配。
 */
int rec_match(const char *seq, const char *const *keys,
              size_t key_count, int tolerance);

#endif /* HUA_RECOGNIZER_H */
