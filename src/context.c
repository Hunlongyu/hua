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
     * 系统通知状态：只有目标本来就是前台窗口时，才用这个全局状态辅助判断。
     * 多显示器上可能是一块屏幕的游戏让系统进入 D3D 全屏，而用户正要在另一块
     * 屏幕的普通窗口上起手；把全局状态无条件套给目标会误伤后者。
     */
    QUERY_USER_NOTIFICATION_STATE state;
    if (hwnd == GetForegroundWindow() &&
        SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_PRESENTATION_MODE)
            return true;
    }

    /*
     * IsZoomed 是 Windows 对“普通最大化”的直接状态，比标题栏样式可靠。
     * 钉钉等自绘标题栏窗口没有 WS_CAPTION；副屏隐藏任务栏后，其最大化矩形又恰好
     * 等于整块显示器，旧逻辑便把它误判成全屏。真正的无边框全屏通常是直接铺设
     * WS_POPUP 窗口而非 SW_MAXIMIZE；D3D/演示全屏已在上面优先识别。
     */
    if (IsZoomed(hwnd))
        return false;

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
