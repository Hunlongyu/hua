/*
 * overlay.c —— 见 overlay.h。
 *
 * 内存策略：不再按整个虚拟桌面分配一张巨型 DIB（4 屏时达 ~30MB 常驻），
 * 而是拆成两个独立的分层窗口，各自按「本帧真正要画的区域」按需分配：
 *   - 轨迹层：按轨迹点包围盒（典型几百 KB）
 *   - OSD 层：按文字实测尺寸（典型几十 KB）
 * 二者分开是关键：OSD 固定在屏底、轨迹可能在屏顶，合成一个窗口的话包围盒
 * 会纵跨整屏，等于没省。淡出结束后释放两个表面，空闲时不占图形内存。
 */
#include "overlay.h"
#include "gdiplus_flat.h"
#include "hua.h"
#include "hook.h"   /* HOOK_MAX_PTS —— 仅用于下面的编译期断言（overlay 在 hook 之上，依赖方向合法） */

#include <math.h>
#include <stdio.h>
#include <wchar.h>

#define OVERLAY_CLASS L"HuaOverlay"
#define FADE_TIMER_ID 1
#define MAX_DRAW_PTS  4096

/*
 * draw_trail 必须画得下 hook 可能交出的全部采样点。
 *
 * 两个常量此前恰好都是 4096，故 draw_trail 的截断分支永不触发——但两者之间没有
 * 任何关联，一旦哪天把 HOOK_MAX_PTS 调大，截断就会生效，且方向是反的：它取的是
 * **最旧**的 MAX_DRAW_PTS 个点、丢掉末端，于是轨迹定格在第 4096 个点、箭头不再
 * 跟手，而 locate_monitor 仍用真末点 → OSD 与轨迹分处两屏。没有任何报错。
 */
_Static_assert(MAX_DRAW_PTS >= HOOK_MAX_PTS,
               "MAX_DRAW_PTS 必须 >= HOOK_MAX_PTS：否则 draw_trail 会丢掉轨迹末端，"
               "表现为手势画一半就卡住且无任何报错");
#define OSD_MAX_GLYPHS 64   /* 动作名/提示文字最多字形数（逐字排版缓冲） */
#define SURF_GRAN     128   /* DIB 尺寸上取整粒度，减少重建次数 */

/* 一个按需分配表面的分层窗口。 */
typedef struct {
    HWND    hwnd;
    HDC     memdc;
    HBITMAP dib, oldbmp;
    void   *bits;
    int     x, y, w, h;       /* 本帧覆盖的屏幕矩形 */
    int     cap_w, cap_h;     /* 当前 DIB 分配尺寸（>= w/h） */
    bool    shown;
} Layer;

static ULONG_PTR g_gdip_token;
static Layer     g_trail_layer;   /* 轨迹 */
static Layer     g_osd_layer;     /* 动作名 / 提示文字 */
static int       g_fade;
static RECT      g_mon;   /* 本次手势所在屏幕的矩形（物理坐标） */

/* 外观（来自 config） */
static bool      g_show_trail = true, g_show_action = true, g_arrow = true, g_random = false;
static unsigned  g_trail_color = 0x00A0FF, g_fail_color = 0x666666;
static int       g_trail_width = 3, g_text_size = 26, g_text_pos = 150;
static int       g_trail_max_len = 2500;   /* 轨迹绘制长度上限（px，0=不限） */
static unsigned  g_text_fill = 0xFFFFFF;   /* OSD 文字镂空填充色 */
static int       g_text_outline = 3;       /* OSD 文字描边宽度（px） */
static int       g_text_spacing = 4;       /* OSD 文字字间距（px） */
/* 上面几个尺寸量是**逻辑像素**（96 DPI 基准）；下面这组是按手势所在屏 DPI 换算出的
 * 物理像素，由 refresh_scaled_metrics 每帧刷新。所有绘制只用这组。 */
static UINT      g_mon_dpi = 96;
static int       g_px_trail_width = 3, g_px_text_size = 26, g_px_text_pos = 150;
static int       g_px_text_outline = 3, g_px_text_spacing = 4;
static unsigned  g_cur_color;   /* 本次手势用色 */
static int       g_color_idx;

/* 绘制点缓冲（图层局部坐标） */
static GpPoint   g_gp[MAX_DRAW_PTS];

static void refresh_scaled_metrics(void);

/* ---------------- 图层：表面按需分配 / 呈现 / 释放 ---------------- */

static void layer_free_surface(Layer *L)
{
    if (L->memdc) {
        if (L->oldbmp)
            SelectObject(L->memdc, L->oldbmp);
        DeleteDC(L->memdc);
        L->memdc = NULL;
        L->oldbmp = NULL;
    }
    if (L->dib) {
        DeleteObject(L->dib);
        L->dib = NULL;
    }
    L->bits = NULL;
    L->cap_w = L->cap_h = 0;
}

/* 令图层覆盖屏幕矩形 (x,y,w,h)；表面不够大时（重新）分配。 */
static bool layer_ensure(Layer *L, int x, int y, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    L->x = x; L->y = y; L->w = w; L->h = h;

    if (L->dib && w <= L->cap_w && h <= L->cap_h)
        return true;   /* 现有表面够用 */

    /* 只增不减（手势结束后整体释放），按粒度上取整避免每帧重建。 */
    int cw = L->cap_w > w ? L->cap_w : w;
    int ch = L->cap_h > h ? L->cap_h : h;
    cw = ((cw + SURF_GRAN - 1) / SURF_GRAN) * SURF_GRAN;
    ch = ((ch + SURF_GRAN - 1) / SURF_GRAN) * SURF_GRAN;

    layer_free_surface(L);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = cw;
    bi.bmiHeader.biHeight      = -ch;   /* 顶向下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    L->memdc = CreateCompatibleDC(screen);
    L->dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &L->bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!L->memdc || !L->dib) {
        layer_free_surface(L);
        return false;
    }
    L->oldbmp = (HBITMAP)SelectObject(L->memdc, L->dib);
    L->cap_w = cw;
    L->cap_h = ch;
    return true;
}

/* 只呈现表面左上角 w×h 区域（表面可能因上取整而更大）。 */
static void layer_present(Layer *L, BYTE const_alpha)
{
    if (!L->hwnd || !L->memdc)
        return;
    POINT dst = { L->x, L->y };
    SIZE  sz  = { L->w, L->h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, const_alpha, AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(L->hwnd, screen, &dst, &sz, L->memdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    if (!L->shown) {
        ShowWindow(L->hwnd, SW_SHOWNA);
        L->shown = true;
    }
}

static void layer_hide(Layer *L)
{
    if (L->hwnd && L->shown) {
        ShowWindow(L->hwnd, SW_HIDE);
        L->shown = false;
    }
}

/* ---------------- 绘制基元 ---------------- */

/* 箭头：尖端在 tip，底边两角在 base 处（即回缩后的线尾），两者对齐无缝。 */
static void draw_arrow(GpGraphics *g, ARGB color, GpPoint base, GpPoint tip)
{
    double dx = tip.X - base.X, dy = tip.Y - base.Y;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1.0)
        return;
    double px = -dy / len, py = dx / len;          /* 垂直单位向量 */
    double half = len * 0.5;                       /* 底边半宽 */

    GpPoint tri[3];
    tri[0] = tip;
    tri[1].X = (INT)(base.X + px * half);
    tri[1].Y = (INT)(base.Y + py * half);
    tri[2].X = (INT)(base.X - px * half);
    tri[2].Y = (INT)(base.Y - py * half);

    GpBrush *br = NULL;
    if (GdipCreateSolidFill(color, &br) == 0 && br) {
        GdipFillPolygonI(g, br, tri, 3, FillModeAlternate);
        GdipDeleteBrush(br);
    }
}

/* 起点用独立实心圆标记，不依赖线帽在不同 GDI+ 实现下的视觉效果。 */
static void draw_start_dot(GpGraphics *g, ARGB color, GpPoint center)
{
    INT diameter = g_px_trail_width * 2 + 2;
    INT radius = diameter / 2;
    GpBrush *br = NULL;
    if (GdipCreateSolidFill(color, &br) == 0 && br) {
        GdipFillEllipseI(g, br, center.X - radius, center.Y - radius,
                         diameter, diameter);
        GdipDeleteBrush(br);
    }
}

/* ---------------- 轨迹层 ---------------- */

static void draw_trail(const Pt *pts, size_t n)
{
    size_t m = n > MAX_DRAW_PTS ? MAX_DRAW_PTS : n;

    /*
     * 蛇形长度上限：从末点往回累计折线长度，超出 TrailMaxLength 的旧段不再绘制，
     * 轨迹像一条定长的蛇跟着光标走。这样一直画也不会让包围盒（进而 DIB）无限增长。
     * 只影响绘制；识别仍用 hook 里的完整轨迹。
     */
    size_t first = 0;
    if (g_trail_max_len > 0) {
        double acc = 0.0;
        size_t i = m - 1;
        for (; i > 0; i--) {
            double dx = (double)pts[i].x - pts[i - 1].x;
            double dy = (double)pts[i].y - pts[i - 1].y;
            double seg = sqrt(dx * dx + dy * dy);
            if (acc + seg > (double)g_trail_max_len)
                break;
            acc += seg;
        }
        first = i;
        /* 至少保留最后一段：单段本身就超过上限时（鼠标极快跳变可产生这种长段），
         * 上面的循环会在第一次迭代就 break 并让 first == m-1，count 退化为 1，
         * 于是整帧不画轨迹、画面闪断。保底留两个点，宁可略微超过长度上限。 */
        if (first > m - 2)
            first = m - 2;
    }
    size_t count = m - first;
    if (count < 2)
        return;

    /* 包围盒（只算要画的那段）+ 余量（线宽 / 起点圆点 / 箭头 / 抗锯齿）。 */
    int minx = pts[first].x, maxx = pts[first].x;
    int miny = pts[first].y, maxy = pts[first].y;
    for (size_t i = first + 1; i < m; i++) {
        if (pts[i].x < minx) minx = pts[i].x;
        if (pts[i].x > maxx) maxx = pts[i].x;
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }
    int margin = g_px_trail_width * 3 + 12;
    int lx = minx - margin, ly = miny - margin;
    int lw = (maxx - minx) + margin * 2, lh = (maxy - miny) + margin * 2;
    if (!layer_ensure(&g_trail_layer, lx, ly, lw, lh))
        return;

    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(g_trail_layer.memdc, &g) != 0 || !g)
        return;
    GdipGraphicsClear(g, 0x00000000);   /* 清为全透明 */
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);

    ARGB color = 0xFF000000u | (g_random ? g_cur_color : g_trail_color);

    for (size_t i = 0; i < count; i++) {
        g_gp[i].X = pts[first + i].x - lx;
        g_gp[i].Y = pts[first + i].y - ly;
    }
    m = count;   /* 以下箭头/绘制逻辑只面向要画的这段 */

    /*
     * 箭头方向不能只取最后两个采样点：StepDistance 去抖后，最后一段
     * 经常短于箭头长度。向前寻找足够远的点，用稳定的末端方向计算箭头，
     * 再把折线截到箭头根部，避免线条穿出箭头尖端。
     */
    bool arrow = g_arrow && m >= 2;
    GpPoint tip = g_gp[m - 1];
    GpPoint base = tip;
    size_t line_count = m;
    if (arrow) {
        double wanted = g_px_trail_width * 3.0 + 8.0;
        size_t anchor = m - 2;
        double dx = 0.0, dy = 0.0, len = 0.0;
        for (;;) {
            dx = (double)tip.X - g_gp[anchor].X;
            dy = (double)tip.Y - g_gp[anchor].Y;
            len = sqrt(dx * dx + dy * dy);
            if (len >= wanted || anchor == 0)
                break;
            anchor--;
        }

        /* 极短轨迹仍按比例画小箭头；完全重合时才无法确定方向。 */
        if (len >= 2.0) {
            double head = len >= wanted ? wanted : len * 0.75;
            base.X = (INT)(tip.X - dx / len * head);
            base.Y = (INT)(tip.Y - dy / len * head);
            line_count = anchor + 2;
            g_gp[line_count - 1] = base;
        } else {
            arrow = false;
        }
    }

    GpPen *pen = NULL;
    if (GdipCreatePen1(color, (REAL)g_px_trail_width, UnitPixel, &pen) == 0 && pen) {
        GdipSetPenStartCap(pen, LineCapRound);
        GdipSetPenEndCap(pen, arrow ? 0 /*平头*/ : LineCapRound);
        GdipSetPenLineJoin(pen, LineJoinRound);
        GdipDrawLinesI(g, pen, g_gp, (INT)line_count);
        GdipDeletePen(pen);
        /* 起点圆点只在这段仍包含真正的手势起点时画；被蛇形截断处不是起点。 */
        if (first == 0)
            draw_start_dot(g, color, g_gp[0]);
        if (arrow)
            draw_arrow(g, color, base, tip);
    }

    GdipDeleteGraphics(g);
    layer_present(&g_trail_layer, 255);
}

/* ---------------- OSD 文字层 ---------------- */

/*
 * 描边=主题色（动作 TrailColor / 未命中 FailColor），字芯=TextFillColor（默认白）镂空。
 * 颜色、描边宽度、字间距均来自配置，代码只提供兜底默认。
 * 先逐字测量墨迹宽度定出图层尺寸，再按 TextLetterSpacing 逐字摆放。
 */
static void draw_osd(const wchar_t *text, bool failed)
{
    GpFontFamily *fam = NULL;
    if (GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fam) != 0 || !fam)
        return;

    GpStringFormat *fmt = NULL;
    GdipCreateStringFormat(0, 0, &fmt);
    if (fmt) {
        GdipSetStringFormatAlign(fmt, StringAlignmentCenter);
        GdipSetStringFormatLineAlign(fmt, StringAlignmentNear);
    }

    int glyphs = (int)wcslen(text);
    if (glyphs > OSD_MAX_GLYPHS)
        glyphs = OSD_MAX_GLYPHS;

    REAL adv[OSD_MAX_GLYPHS];
    REAL total = 0.0f;
    for (int i = 0; i < glyphs; i++) {
        REAL w = (REAL)g_px_text_size;   /* 兜底 */
        GpPath *tmp = NULL;
        if (GdipCreatePath(FillModeAlternate, &tmp) == 0 && tmp) {
            RectF probe = { 0.0f, 0.0f, 10000.0f, 10000.0f };
            RectF b;
            if (GdipAddPathString(tmp, &text[i], 1, fam, FontStyleRegular,
                                  (REAL)g_px_text_size, &probe, fmt) == 0 &&
                GdipGetPathWorldBounds(tmp, &b, NULL, NULL) == 0)
                w = b.Width;
            GdipDeletePath(tmp);
        }
        if (w < 1.0f)                 /* 空格等无墨迹字形：给个近似宽度 */
            w = (REAL)g_px_text_size * 0.3f;
        adv[i] = w;
        total += w;
    }
    total += (REAL)g_px_text_spacing * (glyphs > 0 ? glyphs - 1 : 0);

    /* 图层矩形：在手势所在屏幕水平居中，距该屏底部 g_px_text_pos 像素。
     * 余量兼顾描边、字形超出 advance 的部分与抗锯齿。 */
    int margin = g_px_text_outline + g_px_text_size / 2 + 8;
    int text_x = (g_mon.left + g_mon.right) / 2 - (int)(total / 2.0f);
    int text_y = g_mon.bottom - g_px_text_pos;
    int lx = text_x - margin;
    int ly = text_y - margin;
    int lw = (int)total + margin * 2;
    int lh = g_px_text_size + 12 + margin * 2;
    if (!layer_ensure(&g_osd_layer, lx, ly, lw, lh)) {
        if (fmt)
            GdipDeleteStringFormat(fmt);
        GdipDeleteFontFamily(fam);
        return;
    }

    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(g_osd_layer.memdc, &g) == 0 && g) {
        GdipGraphicsClear(g, 0x00000000);
        GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
        GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);

        ARGB color = 0xFF000000u | (g_random ? g_cur_color : g_trail_color);
        ARGB text_color = failed ? (0xFF000000u | g_fail_color) : color;

        GpPath *path = NULL;
        if (GdipCreatePath(FillModeAlternate, &path) == 0 && path) {
            REAL x = (REAL)margin;              /* 局部坐标：图层原点即 (lx,ly) */
            REAL y = (REAL)margin;
            for (int i = 0; i < glyphs; i++) {
                RectF cell = { x, y, adv[i], (REAL)(g_px_text_size + 12) };
                GdipAddPathString(path, &text[i], 1, fam, FontStyleRegular,
                                  (REAL)g_px_text_size, &cell, fmt);
                x += adv[i] + (REAL)g_px_text_spacing;
            }

            /* 先描边（主题色，可配置宽度），再把字芯填在最上层保证镂空可见。 */
            if (g_px_text_outline > 0) {
                GpPen *outline = NULL;
                if (GdipCreatePen1(text_color, (REAL)g_px_text_outline, UnitPixel,
                                   &outline) == 0 && outline) {
                    GdipSetPenLineJoin(outline, LineJoinRound);
                    GdipDrawPath(g, outline, path);
                    GdipDeletePen(outline);
                }
            }
            GpBrush *fill = NULL;
            if (GdipCreateSolidFill(0xFF000000u | g_text_fill, &fill) == 0 && fill) {
                GdipFillPath(g, fill, path);
                GdipDeleteBrush(fill);
            }
            GdipDeletePath(path);
        }
        GdipDeleteGraphics(g);
        layer_present(&g_osd_layer, 255);
    }

    if (fmt)
        GdipDeleteStringFormat(fmt);
    GdipDeleteFontFamily(fam);
}

/* ---------------- 窗口过程（仅淡出计时） ---------------- */

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TIMER && wParam == FADE_TIMER_ID) {
        g_fade -= 28;
        if (g_fade <= 0) {
            KillTimer(hwnd, FADE_TIMER_ID);
            layer_hide(&g_trail_layer);
            layer_hide(&g_osd_layer);
            /* 手势结束：释放两个表面，空闲时不占图形内存。 */
            layer_free_surface(&g_trail_layer);
            layer_free_surface(&g_osd_layer);
        } else {
            /* 只淡出本轮真正显示着的层。不能无条件 present：layer_present 会把
             * 隐藏的层重新 ShowWindow 出来——若上一手势的 OSD 尚在淡出途中被新一次
             * 点击打断（KillTimer 后 shown 仍为真、旧文字仍留在表面里），而这次点击
             * 是空串走了 layer_hide(OSD)，则这里会把上一条动作名的残影闪回屏幕。 */
            if (g_trail_layer.shown)
                layer_present(&g_trail_layer, (BYTE)g_fade);
            if (g_osd_layer.shown)
                layer_present(&g_osd_layer, (BYTE)g_fade);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---------------- 生命周期 ---------------- */

static HWND create_layer_window(HINSTANCE hinst)
{
    return CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS, NULL, WS_POPUP,
        0, 0, 1, 1, NULL, NULL, hinst, NULL);
}

bool overlay_init(HINSTANCE hinst)
{
    GdiplusStartupInput si;
    ZeroMemory(&si, sizeof(si));
    si.GdiplusVersion = 1;
    if (GdiplusStartup(&g_gdip_token, &si, NULL) != 0)
        return false;

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = overlay_proc;
    wc.hInstance     = hinst;
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);

    /* 默认 OSD 屏幕 = 主屏（首次手势前的兜底）。 */
    g_mon.left = 0;
    g_mon.top = 0;
    g_mon.right = GetSystemMetrics(SM_CXSCREEN);
    g_mon.bottom = GetSystemMetrics(SM_CYSCREEN);

    /* 表面按需分配（见 layer_ensure），这里只建窗口。 */
    g_trail_layer.hwnd = create_layer_window(hinst);
    g_osd_layer.hwnd   = create_layer_window(hinst);
    return g_trail_layer.hwnd != NULL && g_osd_layer.hwnd != NULL;
}

void overlay_shutdown(void)
{
    layer_free_surface(&g_trail_layer);
    layer_free_surface(&g_osd_layer);
    if (g_trail_layer.hwnd) {
        DestroyWindow(g_trail_layer.hwnd);
        g_trail_layer.hwnd = NULL;
    }
    if (g_osd_layer.hwnd) {
        DestroyWindow(g_osd_layer.hwnd);
        g_osd_layer.hwnd = NULL;
    }
    if (g_gdip_token) {
        GdiplusShutdown(g_gdip_token);
        g_gdip_token = 0;
    }
}

void overlay_config(const Config *c)
{
    g_show_trail  = c->show_trail;
    g_show_action = c->show_action_name;
    g_arrow       = c->trail_arrow;
    g_random      = c->random_color;
    g_trail_color = c->trail_color;
    g_fail_color  = c->fail_color;
    /* 这几个是逻辑像素（96 DPI 基准），绘制前由 refresh_scaled_metrics 按屏换算。 */
    g_trail_width  = c->trail_width > 0 ? c->trail_width : 3;
    g_trail_max_len = c->trail_max_length >= 0 ? c->trail_max_length : 0;
    g_text_size    = c->text_size > 0 ? c->text_size : 26;
    g_text_pos     = c->text_position;
    g_text_fill    = c->text_fill_color;
    g_text_outline = c->text_outline_width >= 0 ? c->text_outline_width : 3;
    g_text_spacing = c->text_letter_spacing >= 0 ? c->text_letter_spacing : 0;
    /* 立刻按当前已知 DPI 换算一次：重载配置后到下次手势之间也要是新值。 */
    refresh_scaled_metrics();
}

void overlay_begin(void)
{
    /* 随机色：循环调色板（不用随机数，按次序换色即可）。 */
    static const unsigned palette[] = {
        0x00A0FF, 0xFF5252, 0x4CAF50, 0xFFC107, 0xE040FB, 0x00BCD4,
    };
    if (g_random) {
        /* 先取模再自增：无界自增到 INT_MAX 是有符号溢出（UB），回绕成负数后
         * palette[负数] 就是越界读。虽然要约 2^31 次手势才可达，但零成本可避。 */
        int n = (int)(sizeof(palette) / sizeof(palette[0]));
        g_cur_color = palette[g_color_idx];
        g_color_idx = (g_color_idx + 1) % n;
    } else {
        g_cur_color = g_trail_color;
    }
}

/*
 * 定位手势所在屏幕（用最新点），OSD 就画在这块屏幕上；同时取该屏 DPI。
 *
 * 配置里的 TrailWidth / TextSize / TextPosition 是逻辑像素（以 96 DPI 为基准），
 * 必须按所在屏缩放。进程是 PerMonitorV2，整条坐标链路都是物理像素，若直接把配置
 * 值当物理像素用，同一份配置在 200% 缩放的屏上视觉大小只有 100% 屏的一半。
 *
 * GetDpiForMonitor 在 shcore.dll（Win8.1+）。为不给项目增加导入依赖、也不牺牲
 * 对更早系统的容错，这里动态解析；解析不到就退化为 96（即维持旧行为）。
 */
static UINT monitor_dpi(HMONITOR mon)
{
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
    static GetDpiForMonitorFn fn;
    static bool resolved;

    if (!resolved) {
        resolved = true;
        HMODULE h = GetModuleHandleW(L"shcore.dll");
        if (!h)
            h = LoadLibraryW(L"shcore.dll");
        if (h)
            fn = (GetDpiForMonitorFn)(void *)GetProcAddress(h, "GetDpiForMonitor");
    }
    if (!fn)
        return 96;

    UINT dx = 96, dy = 96;
    if (FAILED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) || dx == 0)
        return 96;
    return dx;
}

/*
 * 逻辑像素 → 当前屏物理像素。非零输入至少返回 ±1，避免线宽/字号被缩成 0。
 *
 * 必须保号：只有 TextPosition 可以是负数（config.c 专门放行 -10000..10000，语义是
 * 把 OSD 放到该屏底边**以下**，用于上下堆叠的多屏布局）。此前的写法
 * `return v > 0 ? v : (logical_px > 0 ? 1 : 0);` 把一切负值塌成 0 —— 与 DPI 无关，
 * 96 DPI 下同样塌 —— 该特性 100% 失效，且 -1 与 -9999 无法区分。
 */
static int scaled(int logical_px)
{
    if (logical_px == 0)
        return 0;
    int v = MulDiv(logical_px, (int)g_mon_dpi, 96);
    if (v == 0)                       /* 缩到 0：保底 ±1，别让非零配置退化成「无」 */
        return logical_px > 0 ? 1 : -1;
    return v;
}

/*
 * 每帧按当前屏 DPI 换算一次，绘制一律用这组物理像素值，而不是在十几个使用点上
 * 分别插缩放——那样必然漏掉某处，且漏掉的地方是「算错」而非「退化」。
 */
static void refresh_scaled_metrics(void)
{
    g_px_trail_width  = scaled(g_trail_width);
    g_px_text_size    = scaled(g_text_size);
    g_px_text_pos     = scaled(g_text_pos);
    g_px_text_outline = scaled(g_text_outline);
    g_px_text_spacing = scaled(g_text_spacing);
}

static void locate_monitor(const Pt *pts, size_t n)
{
    if (n == 0)
        return;
    POINT p = { pts[n - 1].x, pts[n - 1].y };
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi))
        g_mon = mi.rcMonitor;
    g_mon_dpi = monitor_dpi(mon);
    refresh_scaled_metrics();
}

void overlay_update(const Pt *pts, size_t n, const char *seq, const char *action)
{
    if (!g_trail_layer.hwnd)
        return;

    locate_monitor(pts, n);

    /* 实时 OSD：命中 → 动作友好名（TrailColor）；已成方向但无映射 → 「手势无动作」
     * （FailColor）。二者同一时机显示，不必等手势结束。seq 为空（还没画出方向）时
     * 不显示文字，只画轨迹。 */
    wchar_t wtext[320] = L"";
    bool failed = false;
    if (g_show_action) {
        if (action && action[0]) {
            MultiByteToWideChar(CP_UTF8, 0, action, -1, wtext, 320);
        } else if (seq && seq[0]) {
            failed = true;
            wcscpy(wtext, L"手势无动作");
        }
    }

    KillTimer(g_trail_layer.hwnd, FADE_TIMER_ID);   /* 取消可能进行中的淡出 */

    if (g_show_trail && n >= 2)
        draw_trail(pts, n);
    else
        layer_hide(&g_trail_layer);

    if (wtext[0])
        draw_osd(wtext, failed);
    else
        layer_hide(&g_osd_layer);
}

void overlay_end(void)
{
    /* 淡出计时器挂在轨迹层窗口上；没有它就没法 KillTimer，
     * SetTimer(NULL,...) 会创建一个杀不掉的线程计时器，每次调用泄漏一个。 */
    if (!g_trail_layer.hwnd)
        return;
    if (!g_trail_layer.shown && !g_osd_layer.shown)
        return;
    g_fade = 255;
    SetTimer(g_trail_layer.hwnd, FADE_TIMER_ID, 16, NULL);   /* ~150ms 淡出 */
}
