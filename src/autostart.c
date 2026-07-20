/*
 * autostart.c —— 见 autostart.h。用任务计划程序 COM API（ITaskService）实现。
 *
 * 为什么不是 schtasks.exe 命令行（首版做法）：
 *   命令行**根本设不了**下面三项，而它们的默认值对一个常驻托盘程序全是错的
 *   （实测某台机器上首版建出来的任务就是这样）：
 *     ExecutionTimeLimit=PT72H  → 连续跑满 72 小时被任务计划强行终止，表现为"莫名消失"
 *     DisallowStartIfOnBatteries=true → 笔记本用电池时根本不自启
 *     StopIfGoingOnBatteries=true     → 运行中一拔电源就被停掉
 *   只能改走 XML。而 schtasks /create /xml 需要落一个临时 XML 文件，那是提权面：
 *   我们拿它去创建 HighestAvailable 任务，攻击者只要在"写完"与"schtasks 读取"之间
 *   掉包文件内容，就等于让我们替他装了个高权限任务。COM 的 RegisterTask 直接吃
 *   XML **字符串**，压根不落盘，这个窗口不存在。
 *
 * 顺带收益：拿到真正的 HRESULT（首版只能从 schtasks 退出码猜），且不再需要
 * spawn 进程那一套超时/防孤儿的绕行代码。
 */
#include "autostart.h"
#include "platform.h"

#include <windows.h>
#include <taskschd.h>
#include <oleauto.h>
#include <sddl.h>
#include <stdio.h>
#include <wchar.h>

#define TASK_NAME L"hua_autostart"

/* 登录后延迟启动。开机时资源管理器往往还没建好托盘区，NIM_ADD 会失败；虽然 main.c
 * 侧已有"放行 TaskbarCreated + 定时补加"兜底，但让它一次成功更干净。20s 是经验值：
 * 足够覆盖常见开机，又不至于让用户觉得"划不动"。 */
#define LOGON_DELAY L"PT20S"

/* ---------------- 小工具 ---------------- */

/* 取当前用户的 SID 字符串（如 S-1-5-21-...-1001）。用 SID 而非 DOMAIN\user：
 * 免去域名/用户名里的转义与本地化问题。失败返回 false。 */
static bool current_user_sid(wchar_t *out, size_t cap)
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;

    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, NULL, 0, &len);
    if (len == 0) { CloseHandle(tok); return false; }

    TOKEN_USER *tu = (TOKEN_USER *)malloc(len);
    if (!tu) { CloseHandle(tok); return false; }

    bool ok = false;
    if (GetTokenInformation(tok, TokenUser, tu, len, &len)) {
        wchar_t *s = NULL;
        if (ConvertSidToStringSidW(tu->User.Sid, &s) && s) {
            if (wcslen(s) < cap) {
                wcscpy(out, s);
                ok = true;
            }
            LocalFree(s);
        }
    }
    free(tu);
    CloseHandle(tok);
    return ok;
}

/*
 * XML 转义。exe 路径进的是 XML 文本，Windows 路径里 & 是合法字符（如 "D:\R&D\hua.exe"），
 * 不转义会直接生成非法 XML、RegisterTask 报解析错——而且是只有部分用户才碰到的那种。
 */
static bool xml_escape(const wchar_t *in, wchar_t *out, size_t cap)
{
    size_t n = 0;
    for (const wchar_t *p = in; *p; p++) {
        const wchar_t *rep = NULL;
        switch (*p) {
        case L'&':  rep = L"&amp;";  break;
        case L'<':  rep = L"&lt;";   break;
        case L'>':  rep = L"&gt;";   break;
        case L'"':  rep = L"&quot;"; break;
        case L'\'': rep = L"&apos;"; break;
        default: break;
        }
        if (rep) {
            size_t rl = wcslen(rep);
            if (n + rl >= cap) return false;
            wmemcpy(out + n, rep, rl);
            n += rl;
        } else {
            if (n + 1 >= cap) return false;
            out[n++] = *p;
        }
    }
    if (n >= cap) return false;
    out[n] = L'\0';
    return true;
}

/* ---------------- COM 连接 ---------------- */

/*
 * 连上任务计划的根文件夹。成功时 *svc / *folder 由调用方 Release，
 * 且**必须**由调用方按 need_uninit 决定要不要 CoUninitialize。
 */
static HRESULT connect_root(ITaskService **svc, ITaskFolder **folder, bool *need_uninit)
{
    *svc = NULL; *folder = NULL; *need_uninit = false;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* S_FALSE = 本线程已初始化过，仍需配对 CoUninitialize；
     * RPC_E_CHANGED_MODE = 已按别的模型初始化过，此时**不能**去 Uninit 别人的。 */
    if (hr == RPC_E_CHANGED_MODE)
        hr = S_OK;
    else if (SUCCEEDED(hr))
        *need_uninit = true;
    else
        return hr;

    hr = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ITaskService, (void **)svc);
    if (FAILED(hr)) goto fail;

    VARIANT v; VariantInit(&v);   /* 四个 VT_EMPTY = 连本机、当前用户 */
    hr = (*svc)->lpVtbl->Connect(*svc, v, v, v, v);
    if (FAILED(hr)) goto fail;

    BSTR root = SysAllocString(L"\\");
    if (!root) { hr = E_OUTOFMEMORY; goto fail; }
    hr = (*svc)->lpVtbl->GetFolder(*svc, root, folder);
    SysFreeString(root);
    if (FAILED(hr)) goto fail;

    return S_OK;

fail:
    if (*svc) { (*svc)->lpVtbl->Release(*svc); *svc = NULL; }
    if (*need_uninit) { CoUninitialize(); *need_uninit = false; }
    return hr;
}

static void disconnect(ITaskService *svc, ITaskFolder *folder, bool need_uninit)
{
    if (folder) folder->lpVtbl->Release(folder);
    if (svc)    svc->lpVtbl->Release(svc);
    if (need_uninit) CoUninitialize();
}

/* ---------------- 任务 XML ---------------- */

/*
 * 逐项说明「为什么这么设」——这些正是命令行版设不了、且默认值全错的地方：
 *   ExecutionTimeLimit PT0S      不限时。默认 PT72H 会把常驻程序在 72 小时整点杀掉。
 *   DisallowStartIfOnBatteries   false：电池供电也要自启（默认 true = 笔记本上不启动）。
 *   StopIfGoingOnBatteries       false：拔电源不许停我（默认 true = 一拔就没）。
 *   RunOnlyIfIdle / StopOnIdleEnd false：手势工具任何时候都得在。
 *   MultipleInstancesPolicy      IgnoreNew：配合程序自身的单实例互斥量。
 *   LogonTrigger/Delay           见 LOGON_DELAY。
 */
static bool build_task_xml(const wchar_t *exe, const wchar_t *sid,
                           wchar_t *out, size_t cap)
{
    wchar_t exe_esc[MAX_PATH * 6];
    if (!xml_escape(exe, exe_esc, MAX_PATH * 6))
        return false;

    int n = _snwprintf(out, cap,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Description>hua 鼠标手势工具：登录后自动启动</Description>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Triggers>\r\n"
        L"    <LogonTrigger>\r\n"
        L"      <Enabled>true</Enabled>\r\n"
        L"      <UserId>%s</UserId>\r\n"
        L"      <Delay>%s</Delay>\r\n"
        L"    </LogonTrigger>\r\n"
        L"  </Triggers>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <UserId>%s</UserId>\r\n"
        L"      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
        L"    <IdleSettings>\r\n"
        L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n"
        L"      <RestartOnIdle>false</RestartOnIdle>\r\n"
        L"    </IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <WakeToRun>false</WakeToRun>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>7</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>%s</Command>\r\n"
        L"    </Exec>\r\n"
        L"  </Actions>\r\n"
        L"</Task>\r\n",
        sid, LOGON_DELAY, sid, exe_esc);

    if (n < 0 || (size_t)n >= cap)
        return false;   /* _snwprintf 截断时不写终止符，只能整体判失败 */
    return true;
}

/* ---------------- 对外接口 ---------------- */

bool autostart_set(bool enable)
{
    ITaskService *svc = NULL;
    ITaskFolder  *folder = NULL;
    bool uninit = false;
    const char *what = enable ? "创建开机自启任务" : "移除开机自启任务";

    HRESULT hr = connect_root(&svc, &folder, &uninit);
    if (FAILED(hr)) {
        HUA_LOG_W("%s失败：连接任务计划程序失败 hr=0x%08lX"
                  "（常见原因：Task Scheduler 服务被禁用或被组策略/安全软件拦截）",
                  what, (unsigned long)hr);
        return false;
    }

    BSTR name = SysAllocString(TASK_NAME);
    if (!name) {
        disconnect(svc, folder, uninit);
        HUA_LOG_W("%s失败：内存不足", what);
        return false;
    }

    bool ok = false;
    if (!enable) {
        hr = folder->lpVtbl->DeleteTask(folder, name, 0);
        /* 任务本就不存在 = 已达成目标，按成功处理（关自启应当幂等）。 */
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            hr = S_OK;
        ok = SUCCEEDED(hr);
        if (ok)
            HUA_LOG_I("已移除开机自启任务");
        else
            HUA_LOG_W("%s失败：hr=0x%08lX", what, (unsigned long)hr);
    } else {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        /* 截断时返回 cap 并置 ERROR_INSUFFICIENT_BUFFER，必须一并判掉，
         * 否则会拿被截断的路径去建自启任务（与 platform.c 判法一致）。 */
        if (n == 0 || n >= MAX_PATH) {
            HUA_LOG_W("%s失败：exe 路径过长或取不到（n=%lu, err=%lu）",
                      what, n, GetLastError());
        } else {
            wchar_t sid[128];
            if (!current_user_sid(sid, 128)) {
                HUA_LOG_W("%s失败：取当前用户 SID 失败（err=%lu）", what, GetLastError());
            } else {
                static wchar_t xml[8192];   /* 静态：避免在栈上放 16KB */
                if (!build_task_xml(exe, sid, xml, 8192)) {
                    HUA_LOG_W("%s失败：任务 XML 组装失败（路径过长？）", what);
                } else {
                    BSTR bxml = SysAllocString(xml);
                    if (!bxml) {
                        HUA_LOG_W("%s失败：内存不足", what);
                    } else {
                        VARIANT v; VariantInit(&v);   /* 用户/密码/SDDL 均取 XML 里的 Principal */
                        IRegisteredTask *task = NULL;
                        hr = folder->lpVtbl->RegisterTask(
                                folder, name, bxml, TASK_CREATE_OR_UPDATE,
                                v, v, TASK_LOGON_INTERACTIVE_TOKEN, v, &task);
                        SysFreeString(bxml);
                        ok = SUCCEEDED(hr);
                        if (ok) {
                            if (task) task->lpVtbl->Release(task);
                            HUA_LOG_I("已创建开机自启任务（登录后延迟 %ls 启动，不限运行时长，"
                                      "电池供电下同样启动）", LOGON_DELAY);
                        } else {
                            HUA_LOG_W("%s失败：RegisterTask hr=0x%08lX", what,
                                      (unsigned long)hr);
                        }
                    }
                }
            }
        }
    }

    SysFreeString(name);
    disconnect(svc, folder, uninit);
    return ok;
}

bool autostart_exists(void)
{
    ITaskService *svc = NULL;
    ITaskFolder  *folder = NULL;
    bool uninit = false;

    /* 查询失败不记日志：任务不存在是**预期结果**，记告警只会天天喊狼来了。 */
    if (FAILED(connect_root(&svc, &folder, &uninit)))
        return false;

    bool exists = false;
    BSTR name = SysAllocString(TASK_NAME);
    if (name) {
        IRegisteredTask *task = NULL;
        if (SUCCEEDED(folder->lpVtbl->GetTask(folder, name, &task)) && task) {
            exists = true;
            task->lpVtbl->Release(task);
        }
        SysFreeString(name);
    }

    disconnect(svc, folder, uninit);
    return exists;
}
