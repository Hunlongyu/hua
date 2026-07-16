/*
 * hua.h —— 全应用共享的常量与自定义消息定义
 * 保持极小，只放跨模块（main / hook）都要用的东西。
 */
#ifndef HUA_H
#define HUA_H

#include <windows.h>

#include "version.h"

#define HUA_APP_NAME      L"hua"
/* 由 version.h 的数字推导；窄串与宽串拼接后整体为宽串（C11 起有定义）。 */
#define HUA_VERSION       L"" HUA_VERSION_STR
#define HUA_DESCRIPTION   L"Windows 鼠标手势工具"
#define HUA_PROJECT_URL   L"https://github.com/Hunlongyu/hua"
#define HUA_WINDOW_CLASS  L"HuaMessageWindow"
/* 单实例互斥量名。Local\ 前缀 = 每用户会话内唯一即可。 */
#define HUA_MUTEX_NAME    L"Local\\hua_singleton_mutex_9f3c1a"

/* ---- 自定义窗口消息 ---- */
#define WM_HUA_TRAY    (WM_APP + 1)   /* 托盘图标回调消息 */
#define WM_HUA_HOOK    (WM_APP + 2)   /* 钩子线程上报事件；wParam=事件类型（见下） */
#define WM_HUA_RELOAD  (WM_APP + 3)   /* 文件监听线程：ini 变化，请重载 */

/* WM_HUA_HOOK 的 wParam 取值 */
enum {
    HUA_EV_GESTURE_BEGIN = 1,   /* 触发键按下，启动轮询与浮层绘制 */
    HUA_EV_GESTURE_END   = 2,   /* 触发键抬起且识别完成，主线程取方向串→匹配→执行 */
    HUA_EV_GESTURE_CANCEL = 3,  /* 仅普通点击，停止轮询且不执行手势 */
};

/*
 * 合成事件签名：我们用 SendInput 补发右键时，把这个值写进 dwExtraInfo，
 * 钩子回调据此（配合 LLMHF_INJECTED）放行自己的事件，避免递归。
 * 取自 'h''u' → 0x6875，低位留序号。
 */
#define HUA_SIGNATURE  ((ULONG_PTR)0x68750001)

#endif /* HUA_H */
