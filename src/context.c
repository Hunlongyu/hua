/*
 * context.c —— 见 context.h。
 */
#include "context.h"

#include <shlobj.h>   /* SHQueryUserNotificationState / QUERY_USER_NOTIFICATION_STATE */
#include <wctype.h>
#include <string.h>

bool ctx_foreground_exe(HWND hwnd, wchar_t *out, size_t cap)
{
    if (!hwnd || !out || cap == 0)
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid)
        return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return false;

    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, path, &n);
    CloseHandle(h);
    if (!ok)
        return false;

    /* 取 basename，小写复制。 */
    wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;

    size_t i = 0;
    for (; base[i] && i < cap - 1; i++)
        out[i] = towlower(base[i]);
    out[i] = L'\0';
    return true;
}

bool ctx_is_fullscreen(HWND hwnd)
{
    if (!hwnd)
        return false;

    /* 排除桌面外壳（Progman/WorkerW）。 */
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64)) {
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0)
            return false;
    }

    /*
     * 普通最大化窗口的 GetWindowRect 会因不可见 resize border 超出显示器
     * 数个像素，不能据此判成全屏。Chrome/Edge 等自绘标题栏仍保留
     * WS_CAPTION；真正的无边框全屏通常会移除该样式。
     */
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (IsZoomed(hwnd) && (style & WS_CAPTION))
        return false;

    /* 系统通知状态：全屏 D3D / 演示模式 → 视为全屏。 */
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_PRESENTATION_MODE)
            return true;
    }

    /* 几何比对：窗口矩形是否覆盖整块显示器。 */
    RECT wr;
    if (!GetWindowRect(hwnd, &wr))
        return false;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return false;

    return wr.left  <= mi.rcMonitor.left  && wr.top    <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}
