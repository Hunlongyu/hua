/*
 * context.h —— 前台程序识别与全屏检测（Win32）。
 * 手动冒烟测试；per-app 解析/门控的纯逻辑在 config.c（已单测）。
 */
#ifndef HUA_CONTEXT_H
#define HUA_CONTEXT_H

#include <windows.h>
#include <stdbool.h>

/*
 * 取窗口所属进程的 exe 文件名（小写、不含路径）写入 out（宽字符）。
 * 链路：GetWindowThreadProcessId → OpenProcess(QUERY_LIMITED_INFORMATION)
 *       → QueryFullProcessImageNameW → basename → towlower。
 * 成功返回 true。
 */
bool ctx_foreground_exe(HWND hwnd, wchar_t *out, size_t cap);

/*
 * 判断窗口是否处于全屏（独占/无边框全屏）。
 * 优先 SHQueryUserNotificationState，再辅以「窗口矩形覆盖整块显示器且非桌面外壳」。
 */
bool ctx_is_fullscreen(HWND hwnd);

#endif /* HUA_CONTEXT_H */
