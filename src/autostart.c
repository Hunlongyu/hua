/*
 * autostart.c —— 见 autostart.h。首版用 schtasks.exe 简化实现。
 */
#include "autostart.h"
#include "platform.h"   /* HUA_LOG_* —— 本文件此前零日志，失败彻底静默 */

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define TASK_NAME L"hua_autostart"

/* schtasks 冷启动实测约 200ms，慢盘/杀软扫描下更久。留足余量：调用点
 * （autostart_reconcile）已被刻意排在装钩子之前，正是为容忍这段阻塞。 */
#define SCHTASKS_TIMEOUT_MS 15000

/*
 * 静默运行一条命令行。返回 true = 进程正常跑完且退出码为 0。
 *
 * what 仅用于日志；传 NULL = 完全不记日志。此前本函数把 CreateProcessW 的
 * GetLastError() 与 schtasks 的实际退出码全部丢弃、只回一个 bool，于是
 * 「开机自启不工作」在日志里一个字都没有。
 *
 * autostart_exists 必须传 NULL：任务不存在时 schtasks /query 正常退出码非 0，
 * 那是**预期结果**不是错误，记成告警只会天天喊狼来了。
 */
static bool run_hidden(wchar_t *cmdline, const char *what)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (what)
            HUA_LOG_W("%s：无法启动 schtasks.exe，err=%lu", what, GetLastError());
        return false;
    }

    /*
     * 必须判返回值。此前不判：超时后照常 GetExitCodeProcess 拿到 STILL_ACTIVE(259)
     * → 当成「退出码非 0」报失败，然后 CloseHandle 走人，**schtasks 变成孤儿继续跑**
     * 并把任务真的建好了。于是 ini 里写着 AutoStart=false、任务却真实存在，此后每次
     * 启动 autostart_reconcile 都去删它——用户反复开自启、反复失效。
     */
    DWORD w = WaitForSingleObject(pi.hProcess, SCHTASKS_TIMEOUT_MS);
    if (w != WAIT_OBJECT_0) {
        if (what)
            HUA_LOG_W("%s：schtasks 超过 %d ms 未结束（w=%lu, err=%lu），已终止以免留下孤儿进程",
                      what, SCHTASKS_TIMEOUT_MS, w, GetLastError());
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    DWORD code = 1;
    if (!GetExitCodeProcess(pi.hProcess, &code))
        code = 1;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0 && what)
        HUA_LOG_W("%s：schtasks 退出码=%lu（常见原因：任务计划程序服务被禁用或被组策略/"
                  "安全软件拦截）", what, code);
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
    if (n == 0 || n >= MAX_PATH) {
        HUA_LOG_W("设置开机自启失败：exe 路径过长或取不到（n=%lu, err=%lu）",
                  n, GetLastError());
        return false;
    }

    wchar_t sys[MAX_PATH];
    if (!schtasks_path(sys, MAX_PATH)) {
        HUA_LOG_W("设置开机自启失败：无法定位 System32\\schtasks.exe（err=%lu）",
                  GetLastError());
        return false;
    }

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
    return run_hidden(cmd, enable ? "创建开机自启任务" : "移除开机自启任务");
}

bool autostart_exists(void)
{
    wchar_t sys[MAX_PATH];
    if (!schtasks_path(sys, MAX_PATH))
        return false;

    wchar_t cmd[MAX_PATH + 64];
    _snwprintf(cmd, MAX_PATH + 64, L"\"%s\" /query /tn %s", sys, TASK_NAME);
    cmd[MAX_PATH + 63] = L'\0';
    /* 传 NULL：任务不存在时退出码非 0 是预期结果，不是错误。 */
    return run_hidden(cmd, NULL);
}
