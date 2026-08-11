/*
 * recognizer.h —— 轻量几何手势识别，纯逻辑、无 Win32/第三方依赖。
 *
 * 配置仍用九宫格方向串描述手势，但识别不再把输入先压扁成字符串：原始轨迹会经过
 * RDP 简化、转向迟滞和连续角度特征提取，再直接与每个方向模板比较。方向串只用于
 * OSD/日志，因此扇区边界上的一次量化错误不会直接决定最终动作。
 */
#ifndef HUA_RECOGNIZER_H
#define HUA_RECOGNIZER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int x, y;
} Pt;

/* 方向串缓冲约定容量（含结尾 '\0'）。 */
#define REC_MAX_SEQ 64

/*
 * 校验并标准化九宫格模板：只接受 1/2/3/4/6/7/8/9，连续重复方向会折叠
 * （例如 "266" → "26"）。返回标准化后的字符数；无效或缓冲不足返回 0。
 */
size_t rec_normalize_template(const char *key, char *out, size_t out_cap);

/*
 * 匹配诊断。score/second_score 为 0..100；index=-1 表示被最低分或歧义间距拒绝。
 * second_score 在只有一个有效模板时为 0。
 */
typedef struct {
    int index;
    int score;
    int second_score;
} RecMatchResult;

/*
 * 点序列 → 用于显示的 8 方向串。
 *
 * 这不是最终匹配输入，只是把几何分段结果转成可读的九宫格数字。分段包含：
 * - RDP 去除采样锯齿；
 * - 约 22.5 度的同向合并；
 * - FlowMouse 风格、随上一段长度增加的转向确认距离。
 */
size_t rec_encode(const Pt *pts, size_t n, int min_dist,
                  char *out, size_t out_cap);

/* 是否已形成至少一个达到 min_dist 的有效几何线段。 */
bool rec_has_gesture(const Pt *pts, size_t n, int min_dist);

/*
 * 原始轨迹直接匹配 keys[0..key_count)。模板为九宫格方向串。
 *
 * 评分融合连续方向、方向 DTW、相对转角、归一化整体形状和段数差异；不旋转输入，
 * 所以上/下/左/右语义保持不变。min_score 和 ambiguity_margin 均为百分制：最佳分
 * 不足 min_score，或未明显领先第二名 ambiguity_margin，都会拒识。
 */
int rec_match_path(const Pt *pts, size_t n, int min_dist,
                   const char *const *keys, size_t key_count,
                   int min_score, int ambiguity_margin,
                   RecMatchResult *result);

#endif /* HUA_RECOGNIZER_H */
