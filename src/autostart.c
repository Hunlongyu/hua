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

bool autostart_set(bool enable)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return false;

    wchar_t cmd[1400];
    if (enable) {
        /* 登录触发 + 最高权限；/f 覆盖已存在。 */
        _snwprintf(cmd, 1400,
                   L"schtasks /create /tn %s /tr \"\\\"%s\\\"\" /sc onlogon /rl highest /f",
                   TASK_NAME, exe);
    } else {
        _snwprintf(cmd, 1400, L"schtasks /delete /tn %s /f", TASK_NAME);
    }
    return run_hidden(cmd);
}

bool autostart_exists(void)
{
    wchar_t cmd[256];
    _snwprintf(cmd, 256, L"schtasks /query /tn %s", TASK_NAME);
    return run_hidden(cmd);
}
