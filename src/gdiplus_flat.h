/*
 * gdiplus_flat.h —— GDI+ 平面(flat)API 的最小 C 声明子集。
 *
 * 为什么手写：MinGW 的 <gdiplus.h> 是 C++ 专用，其 flat 子头在 C 下也编译不过
 * （REAL 未定义、含 C++ 结构）。我们只用画线/填充/文字这几个函数，故按 gdiplus.dll
 * 稳定的 flat ABI 自行声明，保持项目纯 C。链接 -lgdiplus。
 * 签名与枚举值取自 MinGW 的 gdiplusflat.h / gdiplusenums.h。
 */
#ifndef HUA_GDIPLUS_FLAT_H
#define HUA_GDIPLUS_FLAT_H

#include <windows.h>

typedef float  REAL;
typedef DWORD  ARGB;      /* 0xAARRGGBB */
typedef int    GpStatus;

/* 不透明句柄——只当指针用。 */
typedef void GpGraphics;
typedef void GpPen;
typedef void GpBrush;
typedef void GpFont;
typedef void GpFontFamily;
typedef void GpStringFormat;
typedef void GpPath;

typedef struct { INT  X, Y; }              GpPoint;
typedef struct { REAL X, Y, Width, Height; } RectF;

typedef struct {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} GdiplusStartupInput;

/* 枚举值（取自 gdiplusenums.h） */
enum {
    UnitPixel                  = 2,
    SmoothingModeAntiAlias     = 4,
    TextRenderingHintAntiAlias = 4,
    LineCapRound               = 2,
    LineJoinRound              = 2,
    FontStyleRegular           = 0,
    FillModeAlternate          = 0,
    StringAlignmentNear        = 0,
    StringAlignmentCenter      = 1,
};

GpStatus WINAPI GdiplusStartup(ULONG_PTR*, const GdiplusStartupInput*, void*);
void     WINAPI GdiplusShutdown(ULONG_PTR);

GpStatus WINAPI GdipCreateFromHDC(HDC, GpGraphics**);
GpStatus WINAPI GdipDeleteGraphics(GpGraphics*);
GpStatus WINAPI GdipSetSmoothingMode(GpGraphics*, int);
GpStatus WINAPI GdipSetTextRenderingHint(GpGraphics*, int);
GpStatus WINAPI GdipGraphicsClear(GpGraphics*, ARGB);

GpStatus WINAPI GdipCreatePen1(ARGB, REAL, int /*Unit*/, GpPen**);
GpStatus WINAPI GdipDeletePen(GpPen*);
GpStatus WINAPI GdipSetPenStartCap(GpPen*, int);
GpStatus WINAPI GdipSetPenEndCap(GpPen*, int);
GpStatus WINAPI GdipSetPenLineJoin(GpPen*, int);
GpStatus WINAPI GdipDrawLinesI(GpGraphics*, GpPen*, const GpPoint*, INT);

GpStatus WINAPI GdipCreateSolidFill(ARGB, GpBrush**);
GpStatus WINAPI GdipDeleteBrush(GpBrush*);
GpStatus WINAPI GdipFillEllipseI(GpGraphics*, GpBrush*, INT, INT, INT, INT);
GpStatus WINAPI GdipFillPolygonI(GpGraphics*, GpBrush*, const GpPoint*, INT, int /*FillMode*/);

GpStatus WINAPI GdipCreateFontFamilyFromName(const WCHAR*, void*, GpFontFamily**);
GpStatus WINAPI GdipDeleteFontFamily(GpFontFamily*);
GpStatus WINAPI GdipCreateFont(const GpFontFamily*, REAL, INT, int /*Unit*/, GpFont**);
GpStatus WINAPI GdipDeleteFont(GpFont*);
GpStatus WINAPI GdipDrawString(GpGraphics*, const WCHAR*, INT, const GpFont*,
                               const RectF*, const GpStringFormat*, const GpBrush*);

GpStatus WINAPI GdipCreateStringFormat(INT, LANGID, GpStringFormat**);
GpStatus WINAPI GdipDeleteStringFormat(GpStringFormat*);
GpStatus WINAPI GdipSetStringFormatAlign(GpStringFormat*, int /*StringAlignment*/);
GpStatus WINAPI GdipSetStringFormatLineAlign(GpStringFormat*, int /*StringAlignment*/);

/* GraphicsPath —— 用于描边文字：字形转路径后先描边、再填充。 */
GpStatus WINAPI GdipCreatePath(int /*FillMode*/, GpPath**);
GpStatus WINAPI GdipDeletePath(GpPath*);
GpStatus WINAPI GdipAddPathString(GpPath*, const WCHAR*, INT, const GpFontFamily*,
                                  INT /*style*/, REAL /*emSize*/, const RectF*,
                                  const GpStringFormat*);
GpStatus WINAPI GdipGetPathWorldBounds(GpPath*, RectF*, const void* /*matrix*/,
                                       const GpPen*);
GpStatus WINAPI GdipDrawPath(GpGraphics*, GpPen*, GpPath*);
GpStatus WINAPI GdipFillPath(GpGraphics*, GpBrush*, GpPath*);

#endif /* HUA_GDIPLUS_FLAT_H */
