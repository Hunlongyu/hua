/*
 * autostart.c —— 见 autostart.h。首版用 schtasks.exe 简化实现。
 */
#include "autostart.h"

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define TASK_NAME L"hua_autostart"

/* 静默运行一条命令行，返回退出码是否为 0。 */
static bool run_hidden(wchar_t *cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return false;

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

/*
 * 取 schtasks.exe 的绝对路径。
 *
 * 不能只写 "schtasks"：CreateProcessW 的 lpApplicationName 为 NULL 时，会拿命令行
 * 第一个 token 去按顺序搜索 —— **exe 所在目录与当前工作目录都排在 System32 之前**。
 * 于是 exe 同级（或 CWD）放一个假 schtasks.exe 就会被我们以管理员权限执行。
 * PATH 劫持虽不可行（System32 先于 PATH），但前两条足以构成风险。
 */
static bool schtasks_path(wchar_t *out, size_t cap)
{
    UINT n = GetSystemDirectoryW(out, (UINT)cap);
    if (n == 0 || n >= cap)
        return false;
    /* GetSystemDirectory 不带尾反斜杠 */
    if (_snwprintf(out + n, cap - n, L"\\schtasks.exe") < 0)
        return false;
    out[cap - 1] = L'\0';
    return true;
}

bool autostart_set(bool enable)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    /* 截断时返回 cap 且置 ERROR_INSUFFICIENT_BUFFER，必须一并判掉，
     * 否则会拿一个被截断的路径去建自启任务（与 platform.c 的判法保持一致）。 */
    if (n == 0 || n >= MAX_PATH)
        return false;

    wchar_t sys[MAX_PATH];
    if (!schtasks_path(sys, MAX_PATH))
        return false;

    wchar_t cmd[1400];
    if (enable) {
        /* 登录触发 + 最高权限；/f 覆盖已存在。 */
        _snwprintf(cmd, 1400,
                   L"\"%s\" /create /tn %s /tr \"\\\"%s\\\"\" /sc onlogon /rl highest /f",
                   sys, TASK_NAME, exe);
    } else {
        _snwprintf(cmd, 1400, L"\"%s\" /delete /tn %s /f", sys, TASK_NAME);
    }
    cmd[1399] = L'\0';
    return run_hidden(cmd);
}

bool autostart_exists(void)
{
    wchar_t sys[MAX_PATH];
    if (!schtasks_path(sys, MAX_PATH))
        return false;

    wchar_t cmd[MAX_PATH + 64];
    _snwprintf(cmd, MAX_PATH + 64, L"\"%s\" /query /tn %s", sys, TASK_NAME);
    cmd[MAX_PATH + 63] = L'\0';
    return run_hidden(cmd);
}
