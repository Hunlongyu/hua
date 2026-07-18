/*
 * recognizer.c —— 见 recognizer.h。
 */
#include "recognizer.h"

#include <math.h>
#include <string.h>

/* 扇区索引 → 九宫格方向数字。索引即角度序（每扇 45°，屏幕坐标 y 向下）。 */
static const char kSectorChar[8] = {'6', '3', '2', '1', '4', '7', '8', '9'};
/*                                   0    1    2    3    4    5    6    7
                                     右  右下  下  左下  左  左上  上  右上 */

/* 位移向量 → 扇区索引（0..7）。 */
static int sector_of(int dx, int dy)
{
    const double PI = 3.14159265358979323846;
    double ang = atan2((double)dy, (double)dx);   /* (-PI, PI] */
    if (ang < 0)
        ang += 2.0 * PI;
    return (int)floor(ang / (PI / 4.0) + 0.5) % 8;   /* 就近取扇区 */
}

/*
 * 扇区 a → b 的有符号旋转量，单位=扇区，归一到 (-4, 4]。
 * 正 = 顺索引序（右→右下→下→…），负 = 逆。用来判断「转了多少、往哪边转」。
 */
static int sector_rot(int a, int b)
{
    int r = (b - a) % 8;
    if (r < 0)
        r += 8;          /* 0..7 */
    if (r > 4)
        r -= 8;          /* -3..4 */
    return r;
}

/* 象限 → 对角方向数字。hx:>0=右/<0=左；vy:>0=下/<0=上（屏幕坐标）。 */
static char diag_char(int hx, int vy)
{
    if (hx > 0)
        return vy > 0 ? '3' : '9';
    return vy > 0 ? '1' : '7';
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

    /*
     * 第一步：切成方向段。
     *
     * 关键：段方向取「整段净位移」的量化，而不是各步方向逐一量化后比对。
     * 逐步量化会让贴着扇区边界的笔画（如与水平约成 67.5° 的 V 臂）被几像素的
     * 手抖来回甩过边界，编出 "3232398989" 这种碎串；净位移量化则由整笔的走向
     * 决定，手抖被平均掉，结果稳定且与人的意图一致。
     *
     * 新步要并入当前段，须同时通过两道 <90° 检查，缺一不可：
     *   对「本段首步」(seed)——净方向会随笔画慢慢漂移，只拿它当基准的话，平缓
     *     圆弧每一步都"只差 45°"，一个 90° 圆角会被一路并吞（"26" 退化成 "6"）。
     *     锁定 seed 保证整段相对起手最多张开 ±45°。
     *   对「当前净方向」——只拿 seed 当基准同样会漏：若 seed 恰好落在扇区边界的
     *     另一侧（如 30° 的臂被手抖甩成"右"），则 3(右下) 与 9(右上) 都在它 ±45°
     *     内，整个 V 会被吞成一段"右"。用净方向兜住这种张角过大的并入。
     */
    int    sseed[REC_MAX_SEG];   /* 段首步扇区：并段基准，不随净位移改变 */
    int    ssec[REC_MAX_SEG];    /* 段方向扇区：净位移量化，随并入实时更新 */
    Pt     sbeg[REC_MAX_SEG];    /* 段起点 */
    Pt     send[REC_MAX_SEG];    /* 段终点：保留连续几何信息，供形状判定 */
    double slen[REC_MAX_SEG];    /* 段累计路径长度 */
    int    sc = 0;
    Pt     anchor = pts[0];

    for (size_t i = 1; i < n; i++) {
        int dx = pts[i].x - anchor.x;
        int dy = pts[i].y - anchor.y;
        long long d2 = (long long)dx * dx + (long long)dy * dy;
        if (d2 < min2)
            continue;     /* 距上一采样点位移不足阈值：抖动，忽略 */

        int    sec = sector_of(dx, dy);
        double l   = sqrt((double)d2);

        if (sc > 0) {
            int rs = sector_rot(sseed[sc - 1], sec);
            int rn = sector_rot(ssec[sc - 1], sec);
            if (rs > -2 && rs < 2 && rn > -2 && rn < 2) {   /* 仍是同一笔 */
                slen[sc - 1] += l;
                int ndx = pts[i].x - sbeg[sc - 1].x;
                int ndy = pts[i].y - sbeg[sc - 1].y;
                if (ndx || ndy)
                    ssec[sc - 1] = sector_of(ndx, ndy);
                send[sc - 1] = pts[i];
                anchor = pts[i];
                continue;
            }
        }
        if (sc < REC_MAX_SEG) {
            sseed[sc] = sec;
            ssec[sc]  = sec;
            /*
             * 转弯生出的新段，起点取 pts[i] 而非 anchor：anchor 是转弯前最后一个
             * 采样点，还留在上一笔上（采样间隔 min_dist，真正的拐点落在两者之间）。
             * 若拿它当起点，转弯前那截尾巴会被算进本段净位移——窄 V 上足以把上行臂
             * 的净向量从 上(8) 拽到 右上(9)，编出 "29"。丢掉这一步的代价仅
             * min_dist 像素，且首步方向已记在 ssec 里，段内再并入一步就会被净位移改写。
             *
             * 首段例外：它前面没有"上一笔"，anchor 就是手势起点本身，丢掉这一步会
             * 让净位移少算开头 min_dist 像素，把竖直起手的 L 形拐角算成对角（"26"→"36"）。
             */
            sbeg[sc]  = (sc == 0) ? anchor : pts[i];
            send[sc]  = pts[i];
            slen[sc]  = l;
            sc++;
        }
        anchor = pts[i];
    }

    /*
     * 第二步：删「拐角 / 顶点伪影」。
     *
     * 画折线时锚点还留在上一笔上，等位移攒够 min_dist，锚点→当前点的净向量已经
     * 骑在两笔中间，于是拐点处凭空多出一小段"中间方向"。三种同源表现：
     *   下(2) → [右下(3)] → 右(6)   L 形拐角的斜向伪影
     *   右下(3) → [右(6)] → 右上(9) V 形顶点的水平伪影
     *   下(2) → [右上(9)] → 上(8)   窄 V（近乎原路返回）顶点的伪影
     * 旧规则只认第一种（只删短「对角」段），于是标准 45° V 稳定编成 "369"，
     * 永远匹配不上配置里的 39 = key:f5——越把 V 画标准越是必挂。
     *
     * 三者其实是同一件事：短段 k 落在「前一段 → 后一段」的单调旋转路径上。
     * 判据即：两次转向同号（同一个转弯方向）、合计不超过 180°、且该段够短。
     * 长段不会被删——真正的对角/横向手势本身就长。
     */
    char keep[REC_MAX_SEG];
    for (int k = 0; k < sc; k++)
        keep[k] = 1;
    const double bridge_max = (double)min_dist * 1.6;
    for (int k = 1; k + 1 < sc; k++) {
        if (slen[k] >= bridge_max)
            continue;
        int r1 = sector_rot(ssec[k - 1], ssec[k]);
        int r2 = sector_rot(ssec[k], ssec[k + 1]);
        if (r1 == 0 || r2 == 0)
            continue;                       /* 与邻段同向：交给第三步合并 */
        if ((r1 > 0) != (r2 > 0))
            continue;                       /* 一来一回：真折返，不是伪影 */
        if (r1 + r2 > 4 || r1 + r2 < -4)
            continue;                       /* 转过 180°：不是"夹在中间" */
        keep[k] = 0;
    }

    /*
     * 人眼中的 V 与方向串 28（近似原路下、上）并不是一回事。
     *
     * 八等分扇区会把较陡的 V 臂量化成 2/8，例如真实日志中的右臂与水平约 77°，
     * 明明整条轨迹明显向右展开，却得到 38。这里在伪影过滤后只针对“两段折线”看
     * 连续几何，把它归一到「开口方向两侧的一对对角」。
     *
     * V 的判据（与朝向无关，四个朝向对称处理）：
     *   - 恰好两条可见段；
     *   - 一根「开口轴」上两臂同号（一起撑开），另一根「深度轴」上两臂反号（分叉）；
     *     两根轴上的分量都必须明确——各自超过 min_dist，才算「撑开/分叉」，从而把
     *     L 形（一臂纯竖、一臂纯横，某轴分量≈0）挡在外面；
     *   - 开口跨度 width ≥ 2·min_dist 且 ≥ 高度的 30%（否则是近乎原路返回的 28）；
     *   - 较短臂的深度 ≥ 高度的 40%（否则是「向下再向右、末端略上扬」的 26）。
     * 命中后按每条臂净位移的象限输出对角对：↓↗→39、↓↖→17、↑↘→93、↑↙→71，
     * 以及横向开口的 ∨/∧ 变体。开口轴方向取两臂之和的符号，对「某臂恰好轴对齐」
     * 稳健——分界落在 min_dist 这道有意义的死区上，而不是由 1px 抖动翻面。
     */
    int visible[3];
    int visible_count = 0;
    for (int k = 0; k < sc && visible_count < 3; k++) {
        if (keep[k])
            visible[visible_count++] = k;
    }
    if (visible_count == 2 && out_cap >= 3) {
        int a = visible[0], b = visible[1];
        long long dx1 = (long long)send[a].x - sbeg[a].x;
        long long dy1 = (long long)send[a].y - sbeg[a].y;
        long long dx2 = (long long)send[b].x - sbeg[b].x;
        long long dy2 = (long long)send[b].y - sbeg[b].y;
        const long long clr = min_dist;   /* 「方向明确」的最小分量 */

        int sx1 = dx1 > clr ? 1 : (dx1 < -clr ? -1 : 0);
        int sx2 = dx2 > clr ? 1 : (dx2 < -clr ? -1 : 0);
        int sy1 = dy1 > clr ? 1 : (dy1 < -clr ? -1 : 0);
        int sy2 = dy2 > clr ? 1 : (dy2 < -clr ? -1 : 0);

        /* 开口 X：横向撑开、纵向分叉；开口 Y：纵向撑开、横向分叉。两者互斥。 */
        int open_x = sx1 != 0 && sx1 == sx2 && sy1 != 0 && sy1 == -sy2;
        int open_y = sy1 != 0 && sy1 == sy2 && sx1 != 0 && sx1 == -sx2;

        if (open_x || open_y) {
            long long width, height, shorter;
            int hx1, vy1, hx2, vy2;
            if (open_x) {
                int osign = (dx1 + dx2) > 0 ? 1 : -1;
                long long h1 = dy1 < 0 ? -dy1 : dy1;
                long long h2 = dy2 < 0 ? -dy2 : dy2;
                width   = send[b].x - sbeg[a].x;
                if (width < 0)
                    width = -width;
                height  = h1 > h2 ? h1 : h2;
                shorter = h1 < h2 ? h1 : h2;
                hx1 = hx2 = osign;
                vy1 = sy1;  vy2 = sy2;
            } else {
                int osign = (dy1 + dy2) > 0 ? 1 : -1;
                long long h1 = dx1 < 0 ? -dx1 : dx1;
                long long h2 = dx2 < 0 ? -dx2 : dx2;
                width   = send[b].y - sbeg[a].y;
                if (width < 0)
                    width = -width;
                height  = h1 > h2 ? h1 : h2;
                shorter = h1 < h2 ? h1 : h2;
                vy1 = vy2 = osign;
                hx1 = sx1;  hx2 = sx2;
            }
            if (width >= (long long)min_dist * 2 && height >= (long long)min_dist * 2 &&
                width * 10 >= height * 3 && shorter * 5 >= height * 2) {
                out[0] = diag_char(hx1, vy1);
                out[1] = diag_char(hx2, vy2);
                out[2] = '\0';
                return 2;
            }
        }
    }

    /* 第三步：输出（删段后可能露出相邻同向，需再次合并）。 */
    size_t len = 0;
    int    last = -1;
    for (int k = 0; k < sc; k++) {
        if (!keep[k] || ssec[k] == last)
            continue;
        if (len + 1 < out_cap) {
            out[len++] = kSectorChar[ssec[k]];
            out[len] = '\0';
            last = ssec[k];
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

    /*
     * 空串不是手势，任何 Tolerance 下都不许命中。
     *
     * 放行的话：lev("", "1") == 1，故 Tolerance>=1 时空串命中任意单段模板，且
     * 「平票取短 + 取前」保证必中 [Gestures] 里第一条单字符手势（发布配置中是
     * `1 = cmd:minimize`）。而产生空串根本不需要划手势——状态机在 TriggerDistance(5px)
     * 就进 Active，rec_encode 却要到 MinDistance(20px) 才分出第一段，中间这段「死区」
     * 里编码结果正是空串。于是一次手抖的右键点击就会把当前窗口最小化。
     * 容错的语义是「画歪了也认」，不是「没画也认」。
     */
    if (!seq[0])
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
