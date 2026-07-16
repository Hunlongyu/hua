/*
 * main.c —— app 层：入口 / 消息循环 / 托盘 / 单实例 / 生命周期。
 *
 * 装/卸 WH_MOUSE_LL 钩子；从 hua.ini 加载配置（触发键/阈值/手势表/外观）；
 * 手势结束时匹配 + 执行动作；托盘菜单可重载/打开配置。
 * overlay（轨迹浮层）留待 M5。
 */
#include "hua.h"
#include "hook.h"
#include "platform.h"
#include "recognizer.h"
#include "action.h"
#include "config.h"
#include "context.h"
#include "overlay.h"
#include "autostart.h"
#include "default_ini.h"
#include "resource.h"

#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

#define TIMER_FRAME 1   /* 手势进行中的重绘节流计时器 */

static Config  g_config;
static wchar_t g_ini_path[MAX_PATH];   /* 实际加载到的 ini 路径（空=未找到） */
static char    g_active_exe[CFG_MAX_EXE];  /* 当前手势目标 exe（GESTURE_BEGIN 时缓存） */
static bool    g_active_has_exe;

/* 托盘右键菜单命令 ID */
#define IDM_RELOAD    1001
#define IDM_OPEN_INI  1002
#define IDM_AUTOSTART 1003
#define IDM_PROJECT   1004
#define IDM_EXIT      1005

static HINSTANCE g_hinst;
static HWND      g_hwnd;
static NOTIFYICONDATAW g_nid;

/* ini 文件监听（热加载） */
static HANDLE        g_watch_thread;
static volatile LONG g_watch_stop;
static wchar_t       g_watch_dir[MAX_PATH];
static wchar_t       g_watch_path[MAX_PATH];

typedef struct {
    unsigned long long hash;
    size_t             size;
    bool               valid;
} WatchFingerprint;

static WatchFingerprint g_watch_fingerprint;

/* ---------------- 目标程序 / 门控 ---------------- */

/* 取窗口 exe 名（小写 UTF-8）写入 out；失败返回 false。 */
static bool window_exe_lower(HWND hwnd, char *out, size_t cap)
{
    out[0] = '\0';
    wchar_t w[CFG_MAX_EXE];
    if (!ctx_foreground_exe(hwnd, w, CFG_MAX_EXE))
        return false;
    char *u = hua_utf16_to_utf8(w);   /* exe 名为 ASCII，转码后仍小写 */
    if (!u)
        return false;
    strncpy(out, u, cap - 1);
    out[cap - 1] = '\0';
    hua_free(u);
    return true;
}

/* 取窗口标题并转成 UTF-8，供诊断日志使用。 */
static void window_title_utf8(HWND hwnd, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!hwnd)
        return;

    wchar_t title[256];
    int n = GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));
    if (n <= 0)
        return;

    char *u = hua_utf16_to_utf8(title);
    if (!u)
        return;
    strncpy(out, u, cap - 1);
    out[cap - 1] = '\0';
    hua_free(u);
}

static const char *trigger_name(CfgTrigger trigger)
{
    switch (trigger) {
    case CFG_TRIGGER_RIGHT:  return "right";
    case CFG_TRIGGER_MIDDLE: return "middle";
    case CFG_TRIGGER_X1:     return "x1";
    case CFG_TRIGGER_X2:     return "x2";
    }
    return "unknown";
}

/* 钩子门控：按手势起点下方的程序 + 全屏状态决定是否生效。 */
static bool gesture_gate(HWND target)
{
    char exe[CFG_MAX_EXE];
    bool has = window_exe_lower(target, exe, sizeof(exe));
    bool fs = ctx_is_fullscreen(target);
    return config_app_enabled(&g_config, has ? exe : NULL, fs);
}

/* ---------------- 配置加载 ---------------- */

/* 按当前配置（重新）安装钩子。 */
static void apply_hook_config(void)
{
    hook_uninstall();
    if (!hook_install(g_hwnd, (HuaTrigger)g_config.trigger,
                      g_config.trigger_distance, g_config.min_distance,
                      g_config.step_distance)) {
        HUA_LOG_E("hook_install 失败: %lu", GetLastError());
    }
}

/*
 * 定位配置文件路径，写入 g_ini_path。优先级：
 *   1. exe 同级 hua.ini（便携，最高优先级）
 *   2. %APPDATA%\hua\hua.ini（兜底）
 *   3. 都没有 → 在 AppData 写一份内置默认，再用它
 */
static void resolve_ini_path(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    g_ini_path[0] = L'\0';

    /* 1. exe 同级 */
    if (hua_exe_dir(dir, MAX_PATH)) {
        _snwprintf(path, MAX_PATH, L"%shua.ini", dir);
        if (hua_file_exists(path)) {
            wcsncpy(g_ini_path, path, MAX_PATH - 1);
            return;
        }
    }
    /* 2/3. AppData（不存在则创建默认） */
    if (hua_appdata_dir(dir, MAX_PATH)) {
        _snwprintf(path, MAX_PATH, L"%shua.ini", dir);
        if (!hua_file_exists(path)) {
            /* 写 UTF-8 BOM + 默认内容，便于 Notepad 正确显示中文。 */
            static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            char buf[sizeof(bom) + sizeof(HUA_DEFAULT_INI)];
            memcpy(buf, bom, sizeof(bom));
            memcpy(buf + sizeof(bom), HUA_DEFAULT_INI, sizeof(HUA_DEFAULT_INI) - 1);
            if (hua_write_file(path, buf, sizeof(bom) + sizeof(HUA_DEFAULT_INI) - 1))
                HUA_LOG_I("已在 AppData 创建默认配置");
        }
        wcsncpy(g_ini_path, path, MAX_PATH - 1);
    }
}

static void load_config(void)
{
    resolve_ini_path();

    char *text = g_ini_path[0] ? hua_read_file(g_ini_path) : NULL;
    if (text) {
        config_parse_string(&g_config, text);   /* 内部先 set_defaults */
        hua_free(text);
        char *p8 = hua_utf16_to_utf8(g_ini_path);
        HUA_LOG_I("配置已加载：%d 个全局手势、%d 个程序覆盖（%s）",
                  (int)g_config.gesture_count, (int)g_config.app_count, p8 ? p8 : "");
        hua_free(p8);
    } else {
        /* 文件读写都失败（极少见）：直接解析内置默认文本，保证手势可用。 */
        config_parse_string(&g_config, HUA_DEFAULT_INI);
        HUA_LOG_W("无法读写配置文件，使用内置默认手势（%d 个）",
                  (int)g_config.gesture_count);
    }
    overlay_config(&g_config);   /* 应用外观设置 */
}

/* ---------------- 热加载：监听 ini 目录 ---------------- */

/*
 * FindFirstChangeNotificationW 只能报告“目录内有文件变化”，不能指出文件名。
 * 对实际 ini 内容计算指纹，避免同目录 hua.log、构建产物等写入被误判为配置变化。
 */
static bool watch_read_fingerprint(WatchFingerprint *out)
{
    char *text = hua_read_file(g_watch_path);
    if (!text)
        return false;

    size_t size = strlen(text);
    unsigned long long hash = 14695981039346656037ULL;  /* FNV-1a 64-bit */
    for (size_t i = 0; i < size; i++) {
        hash ^= (unsigned char)text[i];
        hash *= 1099511628211ULL;
    }
    hua_free(text);

    out->hash = hash;
    out->size = size;
    out->valid = true;
    return true;
}

static bool watch_ini_content_changed(void)
{
    WatchFingerprint current = {0};
    if (!watch_read_fingerprint(&current))
        return false;   /* 保存过程中的短暂缺失/占用，等待下一次目录通知 */

    if (!g_watch_fingerprint.valid) {
        g_watch_fingerprint = current;
        return false;
    }

    bool changed = current.size != g_watch_fingerprint.size ||
                   current.hash != g_watch_fingerprint.hash;
    g_watch_fingerprint = current;
    return changed;
}

static DWORD WINAPI watch_proc(LPVOID arg)
{
    (void)arg;
    HANDLE h = FindFirstChangeNotificationW(
        g_watch_dir, FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    /* 建立监听后记录基线，之后只接受 ini 实际内容变化。 */
    ZeroMemory(&g_watch_fingerprint, sizeof(g_watch_fingerprint));
    watch_read_fingerprint(&g_watch_fingerprint);

    while (!g_watch_stop) {
        if (WaitForSingleObject(h, 300) == WAIT_OBJECT_0) {
            /* 先重新挂起通知，再防抖，兼容编辑器“临时文件 + 重命名”保存流程。 */
            FindNextChangeNotification(h);
            Sleep(150);
            if (!g_watch_stop && watch_ini_content_changed())
                PostMessageW(g_hwnd, WM_HUA_RELOAD, 0, 0);
        }
    }
    FindCloseChangeNotification(h);
    return 0;
}

static void watch_start(void)
{
    if (!g_ini_path[0] || g_watch_thread)
        return;
    wcsncpy(g_watch_dir, g_ini_path, MAX_PATH - 1);
    g_watch_dir[MAX_PATH - 1] = L'\0';
    wcsncpy(g_watch_path, g_ini_path, MAX_PATH - 1);
    g_watch_path[MAX_PATH - 1] = L'\0';
    wchar_t *slash = wcsrchr(g_watch_dir, L'\\');
    if (!slash)
        return;
    slash[0] = L'\0';   /* 目录部分 */
    g_watch_stop = 0;
    g_watch_thread = CreateThread(NULL, 0, watch_proc, NULL, 0, NULL);
}

static void watch_stop(void)
{
    if (g_watch_thread) {
        InterlockedExchange(&g_watch_stop, 1);
        WaitForSingleObject(g_watch_thread, 1000);
        CloseHandle(g_watch_thread);
        g_watch_thread = NULL;
    }
}

/* ---------------- 开机自启 ---------------- */

/* 把 ini 里的 AutoStart 行写回为 enable（按行替换，尽量保留其余内容）。 */
static void ini_write_autostart(bool enable)
{
    if (!g_ini_path[0])
        return;
    char *text = hua_read_file(g_ini_path);
    if (!text)
        return;
    FILE *fp = _wfopen(g_ini_path, L"wb");
    if (!fp) {
        hua_free(text);
        return;
    }
    const char *val = enable ? "true" : "false";
    bool replaced = false;
    char *line = text;
    while (*line) {
        char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!replaced && _strnicmp(s, "AutoStart", 9) == 0) {
            fprintf(fp, "AutoStart       = %s\r\n", val);
            replaced = true;
        } else {
            fwrite(line, 1, len, fp);   /* 含行尾 \r */
            if (eol)
                fputc('\n', fp);
        }
        if (!eol)
            break;
        line = eol + 1;
    }
    if (!replaced)
        fprintf(fp, "AutoStart       = %s\r\n", val);
    fclose(fp);
    hua_free(text);
}

/* 启动时把自启任务与配置对账。 */
static void autostart_reconcile(void)
{
    bool exists = autostart_exists();
    if (g_config.auto_start && !exists) {
        if (autostart_set(true)) HUA_LOG_I("已创建开机自启任务");
    } else if (!g_config.auto_start && exists) {
        if (autostart_set(false)) HUA_LOG_I("已移除开机自启任务");
    }
}

/* ---------------- 托盘 ---------------- */

static void tray_add(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_HUA_TRAY;
    /* 托盘用小尺寸图标（从 ico 里选 16px 帧，随 DPI 取 SM_CXSMICON）。 */
    g_nid.hIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"hua · 鼠标手势");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_remove(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void tray_show_menu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_RELOAD,   L"重载配置");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_INI, L"打开配置文件");
    AppendMenuW(menu, MF_STRING | (g_config.auto_start ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_PROJECT,  L"项目地址");
    AppendMenuW(menu, MF_STRING, IDM_EXIT,     L"退出");

    /* 经典技巧：弹菜单前把窗口设为前台，弹完发个空消息，
     * 否则点击菜单外部时菜单不会消失。 */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* ---------------- 窗口过程 ---------------- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_HUA_TRAY:
        /* lParam 携带原始鼠标消息（未升级到 NOTIFYICON_VERSION_4）。 */
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            tray_show_menu(hwnd);
        }
        return 0;

    case WM_HUA_RELOAD:
        /* ini 文件变化：自动重载配置并按新触发键/阈值重装钩子。 */
        load_config();
        apply_hook_config();
        HUA_LOG_I("检测到 ini 变化，已自动重载");
        return 0;

    case WM_HUA_HOOK:
        if (wParam == HUA_EV_GESTURE_BEGIN) {
            /* 从 Down 开始轮询；移动超过阈值后浮层才会实际显示轨迹。 */
            g_active_has_exe = window_exe_lower(hook_last_target(),
                                                g_active_exe, sizeof(g_active_exe));
            overlay_begin();
            SetTimer(hwnd, TIMER_FRAME, 16, NULL);   /* ~60 FPS */
        } else if (wParam == HUA_EV_GESTURE_CANCEL) {
            KillTimer(hwnd, TIMER_FRAME);
            overlay_end();
            hook_replay_trigger_click();
        } else if (wParam == HUA_EV_GESTURE_END) {
            /* 主线程执行：匹配 + 动作（不在钩子回调里做，避免 300ms 超时摘钩）。 */
            KillTimer(hwnd, TIMER_FRAME);

            const Pt *ended_pts = NULL;
            size_t ended_n = hook_snapshot(&ended_pts);
            bool had_trail = ended_n >= 2;
            const char *seq = hook_last_seq();
            HWND target = hook_last_target();
            const char *action = config_resolve(&g_config,
                                                g_active_has_exe ? g_active_exe : NULL, seq);

            /* 用最终 seq 刷新一帧最终判定（命中动作名 / 「手势无动作」），再淡出。
             * 实时帧已在绘制过程中显示，这里只是收尾对齐末点。 */
            overlay_update(ended_pts, ended_n, seq, action ? action_label(action) : NULL);
            overlay_end();

            if (action) {
                HUA_LOG_I("手势 \"%s\" [%s] → %s", seq,
                          g_active_has_exe ? g_active_exe : "?", action);
                action_execute(action, target);
            } else {
                HUA_LOG_I("手势 \"%s\" → 未匹配%s", seq,
                          had_trail ? "（已有轨迹，不补发右键）" : "");
                /* 只有从未形成可见轨迹时才允许还原；长轨迹失败也属于手势。 */
                if (g_config.restore_event && !had_trail)
                    hook_replay_trigger_click();
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_FRAME) {
            /* 像 MouseGestureL 一样主动读取光标，不把轨迹完全押在 Hook Move 上。 */
            hook_poll_cursor();

            /* 截图工具可能接管鼠标并吞掉 Up；无输入超过 PauseTimeout 后主动收尾。 */
            DWORD pause_timeout = g_config.pause_timeout > 0
                                      ? (DWORD)g_config.pause_timeout
                                      : 1000;
            HookTimeoutInfo timeout_info;
            if (hook_cancel_if_timed_out(pause_timeout, &timeout_info)) {
                KillTimer(hwnd, TIMER_FRAME);
                overlay_end();

                const Pt *timed_out_pts = NULL;
                size_t timed_out_n = hook_snapshot(&timed_out_pts);
                char partial_seq[64];
                rec_encode(timed_out_pts, timed_out_n, g_config.min_distance,
                           partial_seq, sizeof(partial_seq));

                char target_exe[CFG_MAX_EXE], down_exe[CFG_MAX_EXE];
                char current_exe[CFG_MAX_EXE];
                char target_title[512], current_title[512];
                bool has_target_exe = window_exe_lower(timeout_info.target,
                                                       target_exe, sizeof(target_exe));
                bool has_down_exe = window_exe_lower(timeout_info.foreground_at_down,
                                                     down_exe, sizeof(down_exe));
                bool has_current_exe = window_exe_lower(timeout_info.foreground_at_timeout,
                                                        current_exe, sizeof(current_exe));
                window_title_utf8(timeout_info.target,
                                  target_title, sizeof(target_title));
                window_title_utf8(timeout_info.foreground_at_timeout,
                                  current_title, sizeof(current_title));

                HUA_LOG_I("手势停顿超时：idle=%llums 达到 PauseTimeout=%lums，已按配置取消且不补发触发键；trigger=%s，轨迹=%s，点数=%zu，起点=(%d,%d)，末点=(%d,%d)，当前光标=(%d,%d)",
                          (unsigned long long)timeout_info.idle_ms,
                          (unsigned long)timeout_info.timeout_ms,
                          trigger_name(g_config.trigger),
                          partial_seq[0] ? partial_seq : "?", timeout_info.point_count,
                          timeout_info.start.x, timeout_info.start.y,
                          timeout_info.last.x, timeout_info.last.y,
                          timeout_info.cursor_valid ? timeout_info.cursor.x : -1,
                          timeout_info.cursor_valid ? timeout_info.cursor.y : -1);
                HUA_LOG_I("超时窗口现场：目标=0x%p [%s] \"%s\"；按下时前台=0x%p [%s]；超时时前台=0x%p [%s] \"%s\"；前台变化=%s",
                          (void *)timeout_info.target,
                          has_target_exe ? target_exe : "?", target_title,
                          (void *)timeout_info.foreground_at_down,
                          has_down_exe ? down_exe : "?",
                          (void *)timeout_info.foreground_at_timeout,
                          has_current_exe ? current_exe : "?", current_title,
                          timeout_info.foreground_at_down == timeout_info.foreground_at_timeout
                              ? "no" : "yes");
                return 0;
            }

            /* 手势进行中：取当前轨迹点，实时编码+解析，驱动浮层重绘。 */
            const Pt *pts = NULL;
            size_t n = hook_snapshot(&pts);
            if (n < 2)
                return 0;
            char seq[64];
            rec_encode(pts, n, g_config.min_distance, seq, sizeof(seq));
            const char *action = config_resolve(&g_config,
                                                g_active_has_exe ? g_active_exe : NULL, seq);
            overlay_update(pts, n, seq, action ? action_label(action) : NULL);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_RELOAD:
            load_config();
            apply_hook_config();
            HUA_LOG_I("菜单: 已重载配置");
            break;
        case IDM_OPEN_INI:
            if (g_ini_path[0])
                ShellExecuteW(NULL, L"open", g_ini_path, NULL, NULL, SW_SHOWNORMAL);
            else
                MessageBoxW(hwnd, L"未找到配置文件 hua.ini。", HUA_APP_NAME,
                            MB_OK | MB_ICONWARNING);
            break;
        case IDM_AUTOSTART: {
            bool enable = !g_config.auto_start;
            if (autostart_set(enable)) {
                g_config.auto_start = enable;
                ini_write_autostart(enable);   /* 持久化到 ini */
                HUA_LOG_I("开机自启：%s", enable ? "开" : "关");
            } else {
                MessageBoxW(hwnd, L"设置开机自启失败（需要管理员权限）。",
                            HUA_APP_NAME, MB_OK | MB_ICONWARNING);
            }
            break;
        }
        case IDM_PROJECT:
            ShellExecuteW(NULL, L"open", HUA_PROJECT_URL, NULL, NULL, SW_SHOWNORMAL);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---------------- 入口 ---------------- */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nShowCmd)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nShowCmd;
    g_hinst = hInstance;

    /* 单实例：命名互斥量。已存在则前置提示并退出。 */
    HANDLE mutex = CreateMutexW(NULL, TRUE, HUA_MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"hua 已在运行。", HUA_APP_NAME, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        return 0;
    }

    hua_log_init();

    /* 注册并创建 message-only 窗口（无可见窗口，但能跑消息循环 + 收托盘/钩子消息）。 */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = HUA_WINDOW_CLASS;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!RegisterClassExW(&wc)) {
        HUA_LOG_E("RegisterClassExW 失败: %lu", GetLastError());
        goto cleanup_mutex;
    }

    g_hwnd = CreateWindowExW(0, HUA_WINDOW_CLASS, HUA_APP_NAME,
                             0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, hInstance, NULL);
    if (!g_hwnd) {
        HUA_LOG_E("CreateWindowExW 失败: %lu", GetLastError());
        goto cleanup_mutex;
    }

    tray_add(g_hwnd);

    if (!overlay_init(hInstance))
        HUA_LOG_W("overlay 初始化失败（浮层不可用，不影响手势）");

    hook_set_gate(gesture_gate);
    load_config();
    apply_hook_config();
    autostart_reconcile();
    watch_start();
    HUA_LOG_I("hua 就绪（识别闭环 + per-app 门控 + 浮层 + 热加载）");

    /* 消息循环 */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    /* 退出清理 */
    watch_stop();
    hook_uninstall();
    overlay_shutdown();
    tray_remove();
    hua_log_close();

cleanup_mutex:
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
