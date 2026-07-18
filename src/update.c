/*
 * update.c —— hua 自动更新实现。见 update.h。
 */
#include "update.h"
#include "hua.h"
#include "platform.h"
#include "win_autoupdate.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 当前构建的架构串。发布产物按架构命名（hua-v1.0.9-x64.exe / -x86.exe / -arm64.exe），
 * 每个架构的 exe 只应更新到同架构资产——否则会给 ARM 设备装上 x64 包。 */
#if defined(_M_ARM64) || defined(__aarch64__)
#  define HUA_ARCH "arm64"
#elif defined(_M_X64) || defined(__x86_64__)
#  define HUA_ARCH "x64"
#elif defined(_M_IX86) || defined(__i386__)
#  define HUA_ARCH "x86"
#else
#  error "未知架构：无法确定自动更新的资产名"
#endif

/* hua 的仓库与资产约定。EXE 单文件模式：便携程序，原子 rename 替换最干净。 */
#define UPD_OWNER          "Hunlongyu"
#define UPD_REPO           "hua"
#define UPD_ASSET_PATTERN  "hua-*-" HUA_ARCH ".exe"

/* UTF-8 → 新分配宽字符串（调用方 free）。updater 的对外 API 用 UTF-8，此处转宽显示。 */
static wchar_t *u8_to_w(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    return w;
}

/* 发现新版后，供主线程弹气泡用的版本串（后台线程写、主线程读；PostMessage 前才写、
 * 消息处理时才读，时序上无竞争）。 */
static wchar_t g_found_ver[64];

/* updater 的日志回调 → 转接 hua 日志系统。 */
static void on_log(au_log_level_t lv, const char *msg, void *ud) {
    (void)ud;
    switch (lv) {
        case AU_LOG_ERROR: HUA_LOG_E("[update] %s", msg); break;
        case AU_LOG_WARN:  HUA_LOG_W("[update] %s", msg); break;
        default:           HUA_LOG_I("[update] %s", msg); break;
    }
}

/* 下载进度 → 写进日志（无控制台，粗粒度即可，避免刷屏）。 */
static int on_progress(uint64_t total, uint64_t done, void *ud) {
    int *last = (int *)ud;
    int pct = total ? (int)(done * 100 / total) : -1;
    if (pct < 0) return 0;
    if (pct >= *last + 25 || pct == 100) {   /* 每 25% 记一次 */
        *last = pct;
        HUA_LOG_I("[update] 下载 %d%%", pct);
    }
    return 0;
}

/* 线程参数：快照必要配置，避免与 g_config 的热重载竞争。 */
typedef struct {
    HWND             hwnd;
    int              manual;
    CfgUpdateChannel channel;
} UpdateJob;

static au_ctx_t *make_ctx(const UpdateJob *job, int *progress_last) {
    au_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.owner           = UPD_OWNER;
    cfg.repo            = UPD_REPO;
    cfg.current_version = HUA_VERSION_STR;   /* 窄串，如 "1.0.9" */
    cfg.asset_pattern   = UPD_ASSET_PATTERN;
    cfg.mode            = AU_MODE_EXE;
    cfg.channel         = (job->channel == CFG_CHANNEL_BETA) ? AU_CHANNEL_BETA : AU_CHANNEL_STABLE;
    cfg.on_log          = on_log;
    cfg.on_progress     = on_progress;
    cfg.userdata        = progress_last;
    return au_create(&cfg);
}

static DWORD WINAPI update_thread(LPVOID param) {
    UpdateJob *job = (UpdateJob *)param;
    int progress_last = 0;
    au_ctx_t *ctx = make_ctx(job, &progress_last);
    if (!ctx) {
        if (job->manual)
            MessageBoxW(NULL, L"更新模块初始化失败。", HUA_APP_NAME, MB_OK | MB_ICONERROR);
        free(job);
        return 1;
    }

    au_release_t *rel = NULL;
    au_err_t e = au_fetch_latest(ctx, &rel);
    if (e != AU_OK) {
        HUA_LOG_W("[update] 检查失败: %s", au_strerror(e));
        if (job->manual) {
            wchar_t msg[256];
            _snwprintf(msg, 256, L"检查更新失败：%hs\n\n请检查网络后重试。", au_strerror(e));
            MessageBoxW(NULL, msg, HUA_APP_NAME, MB_OK | MB_ICONWARNING);
        }
        au_destroy(ctx);
        free(job);
        return 0;   /* 检查失败绝不阻塞程序 */
    }

    int cmp = au_compare_current(ctx, rel);
    if (cmp <= 0) {
        HUA_LOG_I("[update] 已是最新（当前 %s，最新 %hs）", HUA_VERSION, rel->ver.raw);
        if (job->manual) {
            wchar_t msg[128];
            _snwprintf(msg, 128, L"已是最新版本 v%s。", HUA_VERSION);
            MessageBoxW(NULL, msg, HUA_APP_NAME, MB_OK | MB_ICONINFORMATION);
        }
        au_release_free(rel);
        au_destroy(ctx);
        free(job);
        return 0;
    }

    /* 发现新版本。 */
    wchar_t *wver = u8_to_w(rel->ver.raw);

    if (!job->manual) {
        /* 后台静默检查：仅提示，不下载。请主线程弹气泡。 */
        _snwprintf(g_found_ver, 64, L"%s", wver ? wver : L"");
        g_found_ver[63] = L'\0';
        PostMessageW(job->hwnd, WM_HUA_UPDATE, 0, 0);
    } else {
        /* 手动：确认 → 下载 → 替换重启。 */
        wchar_t *wlog = rel->changelog ? u8_to_w(rel->changelog) : NULL;
        wchar_t prompt[2048];
        _snwprintf(prompt, 2048,
                   L"发现新版本 %s（当前 v%s）。\n\n更新日志：\n%s\n\n现在下载并更新吗？\n"
                   L"（更新完成后 hua 会自动重启）",
                   wver ? wver : L"", HUA_VERSION, wlog ? wlog : L"（无）");
        prompt[2047] = L'\0';
        free(wlog);

        int yes = MessageBoxW(NULL, prompt, HUA_APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES;
        if (yes) {
            const au_asset_t *a = au_pick_asset(rel, UPD_ASSET_PATTERN);
            if (!a) {
                wchar_t msg[128];
                _snwprintf(msg, 128, L"未找到匹配当前架构的更新资产（%hs）。", UPD_ASSET_PATTERN);
                MessageBoxW(NULL, msg, HUA_APP_NAME, MB_OK | MB_ICONERROR);
            } else {
                char path[512];
                e = au_download(ctx, a, path, sizeof path);
                if (e != AU_OK) {
                    wchar_t msg[256];
                    _snwprintf(msg, 256, L"下载失败：%hs", au_strerror(e));
                    MessageBoxW(NULL, msg, HUA_APP_NAME, MB_OK | MB_ICONERROR);
                } else {
                    HUA_LOG_I("[update] 下载完成，正在应用更新并重启...");
                    e = au_apply(ctx, path);   /* 成功则不返回：进程已重启 */
                    wchar_t msg[256];
                    _snwprintf(msg, 256, L"应用更新失败：%hs", au_strerror(e));
                    MessageBoxW(NULL, msg, HUA_APP_NAME, MB_OK | MB_ICONERROR);
                }
            }
        }
    }

    free(wver);
    au_release_free(rel);
    au_destroy(ctx);
    free(job);
    return 0;
}

static void spawn(HWND hwnd, const Config *cfg, int manual) {
    UpdateJob *job = (UpdateJob *)malloc(sizeof *job);
    if (!job) return;
    job->hwnd    = hwnd;
    job->manual  = manual;
    job->channel = cfg->update_channel;
    HANDLE t = CreateThread(NULL, 0, update_thread, job, 0, NULL);
    if (t) CloseHandle(t);
    else   free(job);
}

void update_boot(void) {
    /* argv 从宽字符命令行取（hua 走 WinMain，lpCmdLine 是 ANSI，不能用）。 */
    int argc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wargv) {
        au_wait_for_predecessor_w(argc, wargv, 10000);
        LocalFree(wargv);
    }
    au_cleanup_after_restart();
}

void update_start_background_check(HWND hwnd, const Config *cfg) {
    if (!cfg->update_enabled || !cfg->update_auto_check) return;
    spawn(hwnd, cfg, 0);
}

void update_check_now(HWND hwnd, const Config *cfg) {
    if (!cfg->update_enabled) {
        MessageBoxW(NULL, L"自动更新已在配置中禁用（[Update] Enabled=false）。",
                    HUA_APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }
    spawn(hwnd, cfg, 1);
}

void update_on_found_message(HWND hwnd) {
    /* 在主线程用托盘气泡提示（避免抢焦点的 MessageBox）。 */
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd   = hwnd;
    nid.uID    = 1;                 /* 与 tray_add 的 uID 一致 */
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy(nid.szInfoTitle, L"hua 有新版本");
    _snwprintf(nid.szInfo, sizeof(nid.szInfo)/sizeof(wchar_t),
               L"发现 %s（当前 v%s）。右键托盘图标 → 检查更新 即可升级。",
               g_found_ver, HUA_VERSION);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
