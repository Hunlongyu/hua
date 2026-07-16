/*
 * overlay.h —— 屏幕浮层：GDI+ 抗锯齿轨迹线 + 动作名 OSD + 淡出。
 *
 * 一个覆盖整个虚拟桌面的分层窗口（WS_EX_LAYERED|TRANSPARENT|TOPMOST|
 * NOACTIVATE|TOOLWINDOW），逐像素 alpha。绘制发生在主线程（钩子只采点）。
 * 消费 config 的轨迹/文字/颜色等外观项。
 */
#ifndef HUA_OVERLAY_H
#define HUA_OVERLAY_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "recognizer.h"   /* Pt */

bool overlay_init(HINSTANCE hinst);
void overlay_shutdown(void);

/* 应用外观配置（颜色/线宽/箭头/字号/位置/开关）。 */
void overlay_config(const Config *c);

/* 手势开始：准备一次新绘制（如随机色则换色）。 */
void overlay_begin(void);

/* 增量重绘：pts/n 当前轨迹点（物理屏幕坐标）；seq 方向串；action 命中动作名（可 NULL）。
 * 命中显示动作名；seq 非空但无命中显示「手势无动作」；seq 为空只画轨迹。 */
void overlay_update(const Pt *pts, size_t n, const char *seq, const char *action);

/* 手势结束：淡出隐藏。 */
void overlay_end(void);

#endif /* HUA_OVERLAY_H */
