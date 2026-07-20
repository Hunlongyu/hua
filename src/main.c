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
#include "ini_edit.h"
#include "resource.h"
#include "update.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#define TIMER_FRAME    1   /* 手势进行中的重绘节流计时器 */
#define TIMER_WATCHDOG 2   /* 钩子探活计时器 */
#define TIMER_TRAY_RETRY 3 /* 托盘图标补加计时器（见 tray_add 失败处） */

/* 开机自启时资源管理器可能还没建好托盘区，NIM_ADD 会失败。理论上等它广播
 * TaskbarCreated 即可重建，但那条广播并不保证送达（见 WinMain 里的 UIPI 说明），
 * 而托盘是本程序唯一的 UI 入口，丢了就再也没法重载配置或退出。故失败后自行重试兜底。
 * 2s × 90 ≈ 3 分钟，足够覆盖慢盘/杀软扫描下的开机；超时仍失败则记 ERROR 收手。 */
#define TRAY_RETRY_INTERVAL_MS    2000
#define TRAY_RETRY_MAX_ATTEMPTS   90

#define WATCHDOG_INTERVAL_MS      3000    /* 探活周期 */
#define WATCHDOG_LOG_THROTTLE_MS  60000   /* 重装日志限频（见 TIMER_WATCHDOG 分支） */

static Config  g_config;
static wchar_t g_ini_path[MAX_PATH];   /* 实际加载到的 ini 路径（空=未找到） */
static char    g_active_exe[CFG_MAX_EXE];  /* 当前手势目标 exe（GESTURE_BEGIN 时缓存） */
static bool    g_active_has_exe;
static ULONGLONG g_last_revive_log;   /* 上次打印钩子重装日志的时刻（限频用） */

/* 托盘右键菜单命令 ID */
#define IDM_RELOAD    1001
#define IDM_OPEN_INI  1002
#define IDM_AUTOSTART 1003
#define IDM_PROJECT   1004
#define IDM_EXIT      1005
#define IDM_UPDATE    1006

static HINSTANCE g_hinst;
static HWND      g_hwnd;
static NOTIFYICONDATAW g_nid;
static bool      g_icon_owned;   /* g_nid.hIcon 是否由我们拥有（需 DestroyIcon） */
/* 资源管理器重建托盘区（重启，或开机时晚于我们启动）后会向所有【顶层】窗口广播此消息，
 * 届时托盘图标需要重建，否则图标永久消失——而托盘是本程序唯一的 UI 入口。
 * 主窗口因此刻意建成顶层窗口而非 HWND_MESSAGE（后者收不到广播），见 WinMain 处注释。 */
static UINT      g_wm_taskbar_created;

/* ini 文件监听（热加载） */
static HANDLE        g_watch_thread;
/* 停止信号。有了它，监听线程就能在「目录有变化」与「该退了」之间无超时地等待——
 * 既不必每 300ms 醒来查一眼是否该停（这个进程绝大多数时间是完全空闲的），
 * 退出也从「最长等一整轮」变成即时。手动重置：一次 Set 要能叫醒并停住线程。 */
static HANDLE        g_watch_stop_evt;
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
    /* 目标是我们自己的窗口时一律不接管。托盘菜单弹出期间照样能划手势，而目标经
     * GA_ROOTOWNER 会解析到 g_hwnd —— 在自己的菜单上划「下右」就会对自己执行
     * cmd:close_window，程序莫名其妙自我关闭。自身窗口上的手势没有任何意义。 */
    if (target == g_hwnd)
        return false;

    char exe[CFG_MAX_EXE];
    bool has = window_exe_lower(target, exe, sizeof(exe));
    bool fs = ctx_is_fullscreen(target);
    return config_app_enabled(&g_config, has ? exe : NULL, fs);
}

/* ---------------- 配置加载 ---------------- */

/* 按当前配置（重新）安装钩子。
 *
 * 重装会把状态机强制掰回 ST_IDLE，若此刻正有手势在进行（用户改完 ini 保存后
 * 立刻划一下试效果，是很自然的操作），主线程侧的 TIMER_FRAME 就再也等不到
 * END/CANCEL 事件：定时器无人回收、浮层永不淡出，一条 TOPMOST 轨迹会永久
 * 糊在屏幕上并持续 60fps 空烧。故此处必须主动收尾。 */
static void apply_hook_config(void)
{
    if (!g_hwnd)
        return;   /* 无窗口则钩子也没有通知目标，装了也白装 */
    KillTimer(g_hwnd, TIMER_FRAME);
    overlay_end();
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
    /* _snwprintf 截断时不写终止符，必须手动补。注意此处「不会截断」只是巧合：
     * hua_exe_dir 保证 len(dir)+len("hua.exe") <= 259，而 "hua.ini" 恰好同为 7 字符，
     * 一旦 exe 改名成更短的名字，拼接就会溢出。不能依赖这个巧合。 */
    if (hua_exe_dir(dir, MAX_PATH)) {
        _snwprintf(path, MAX_PATH, L"%shua.ini", dir);
        path[MAX_PATH - 1] = L'\0';
        if (hua_file_exists(path)) {
            wcsncpy(g_ini_path, path, MAX_PATH - 1);
            g_ini_path[MAX_PATH - 1] = L'\0';   /* wcsncpy 不保证终止；勿依赖 static 零初始化 */
            return;
        }
    }
    /* 2/3. AppData（不存在则创建默认） */
    if (hua_appdata_dir(dir, MAX_PATH)) {
        _snwprintf(path, MAX_PATH, L"%shua.ini", dir);
        path[MAX_PATH - 1] = L'\0';
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
        g_ini_path[MAX_PATH - 1] = L'\0';
    }
}

static void apply_log_config(const Config *c)
{
    HuaLogLevel level = c->log_enabled ? (HuaLogLevel)c->log_level : HUA_LOG_OFF;
    hua_log_configure(level, c->log_max_size_mb, c->log_retention_days);
}

/*
 * 日志必须在正式初始化前先读到策略：否则 LogEnabled=false 的每次启动仍会先创建
 * 文件并写一行启动日志，语义上并没有真正关闭。这里只做一次无副作用的预解析；
 * load_config 稍后仍负责完整诊断、外观应用与热加载。
 */
static void configure_log_before_init(void)
{
    resolve_ini_path();
    /* Config 内含完整 per-app 手势表，超过默认 1 MB 主线程栈，必须放静态存储。 */
    static Config bootstrap;
    char *text = g_ini_path[0] ? hua_read_file(g_ini_path, NULL) : NULL;
    if (text) {
        config_parse_string(&bootstrap, text);
        hua_free(text);
    } else {
        config_set_defaults(&bootstrap);
    }
    apply_log_config(&bootstrap);
}

/*
 * 把 config 攒下的解析诊断打出来。config 模块零 Win32 依赖、不能自己打日志，
 * 故它只计数，由这里翻译成人话。三类问题此前都是彻底静默的：用户拼错一个键、
 * 写了个 `ShowTrail = 真`、或手势超过 128 条，都只会表现为「这项设置莫名不生效」。
 */
static void log_config_diag(const Config *c)
{
    const char *first = c->diag.first_issue[0] ? c->diag.first_issue : "?";
    if (c->diag.unknown_keys > 0)
        HUA_LOG_W("配置中有 %d 个无法识别的键（首个：%s），已忽略——请检查拼写",
                  c->diag.unknown_keys, first);
    if (c->diag.bad_values > 0)
        HUA_LOG_W("配置中有 %d 个值无法识别（首个：%s），已按默认值处理；"
                  "布尔开关接受 true/false（也认 1/0、yes/no、on/off），"
                  "日志等级接受 off/error/warn/info",
                  c->diag.bad_values, first);
    if (c->diag.dropped > 0)
        HUA_LOG_W("配置超出容量上限，已丢弃 %d 项（首个：%s）；上限：全局手势 %d 条、"
                  "程序节 %d 个、每程序手势 %d 条",
                  c->diag.dropped, first,
                  CFG_MAX_GESTURES, CFG_MAX_APPS, CFG_MAX_APP_GESTURES);
}

static void load_config(void)
{
    resolve_ini_path();

    size_t raw_len = 0;
    char *text = g_ini_path[0] ? hua_read_file(g_ini_path, &raw_len) : NULL;
    if (text) {
        /*
         * 含嵌入 NUL 几乎必然意味着 ini 被存成了 UTF-16（记事本「另存为 Unicode」）。
         * inih 按 UTF-8 逐字节解析，会在第一个 NUL 处就结束 —— 结果是**静默**得到
         * 零手势：用户发现手势全没了，日志却只有一条平静的「已加载 0 个手势」。
         * 必须明确告诉他原因，否则无从排查。
         */
        if (raw_len != strlen(text))
            HUA_LOG_W("配置疑似 UTF-16 编码（含嵌入 NUL），只能解析到前 %zu/%zu 字节，"
                      "手势多半会全部失效；请将 hua.ini 另存为 UTF-8。",
                      strlen(text), raw_len);

        /* 坏行会被 inih 跳过，但必须告诉用户——否则那条手势只是「莫名其妙不生效」。
         * 注意超长行（> INI_MAX_LINE）是被截断后照常解析的，不是整行丢弃。 */
        int bad_line = 0;
        config_parse_string_ex(&g_config, text, &bad_line);   /* 内部先 set_defaults */
        /* 热加载时先应用日志策略：关闭后本次 reload 不再多写尾巴，开启则立即生效。 */
        apply_log_config(&g_config);
        if (bad_line > 0)
            HUA_LOG_W("配置第 %d 行解析有误（该行已跳过；也可能是行超长被截断）", bad_line);
        log_config_diag(&g_config);
        hua_free(text);
        char *p8 = hua_utf16_to_utf8(g_ini_path);
        HUA_LOG_I("配置已加载：%d 个全局手势、%d 个程序覆盖（%s）",
                  (int)g_config.gesture_count, (int)g_config.app_count, p8 ? p8 : "");
        hua_free(p8);
    } else {
        /* 文件读写都失败（极少见）：直接解析内置默认文本，保证手势可用。 */
        config_parse_string(&g_config, HUA_DEFAULT_INI);
        apply_log_config(&g_config);
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
    /* 必须按真实字节数算指纹，不能用 strlen：若 ini 被存成 UTF-16，其每个 ASCII
     * 字符后都跟一个 \0，strlen 会在第一个字符处就截断，此后的任何改动都算不进
     * 指纹里 —— 热加载会静默失效。 */
    size_t size = 0;
    char *text = hua_read_file(g_watch_path, &size);
    if (!text)
        return false;

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
    if (h == INVALID_HANDLE_VALUE) {
        /* 静默退出会让热加载永久失效，而 g_watch_thread 仍非空 → watch_start 以为
         * 监听还活着，用户也完全无从察觉。至少要留下日志。 */
        HUA_LOG_W("监听配置目录失败: %lu（热加载不可用，请用托盘「重载配置」）",
                  GetLastError());
        return 0;
    }

    /* 建立监听后记录基线，之后只接受 ini 实际内容变化。 */
    ZeroMemory(&g_watch_fingerprint, sizeof(g_watch_fingerprint));
    watch_read_fingerprint(&g_watch_fingerprint);

    /*
     * 同时等「目录有变化」与「该退了」，无超时。
     *
     * 此前是 WaitForSingleObject(h, 300) 轮询：这个进程 99.9% 的时间完全空闲，却
     * 每秒醒 3.3 次，只为看一眼是否该停。改成等两个句柄后，空闲期零唤醒，
     * 且 watch_stop 不再需要等满一整轮（WaitForSingleObject(300) + Sleep(150) + 读文件）。
     */
    HANDLE waits[2] = { h, g_watch_stop_evt };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0)
            break;   /* 停止信号，或等待失败（句柄失效）——两种都该收摊 */

        /* 先重新挂起通知，再防抖，兼容编辑器“临时文件 + 重命名”保存流程。
         * 防抖期间也要能被叫醒退出，故用等停止事件代替 Sleep。 */
        FindNextChangeNotification(h);
        if (WaitForSingleObject(g_watch_stop_evt, 150) == WAIT_OBJECT_0)
            break;
        if (watch_ini_content_changed())
            PostMessageW(g_hwnd, WM_HUA_RELOAD, 0, 0);
    }
    FindCloseChangeNotification(h);
    return 0;
}

static bool watch_stop(void);

/* 启动（或在 ini 路径变化后重启）配置监听。可重复调用：路径未变则原样返回。 */
static void watch_start(void)
{
    if (!g_ini_path[0])
        return;
    if (g_watch_thread) {
        /* load_config 每次都会重新解析 ini 路径，它是可能变的——例如用户在运行期
         * 于 exe 同级新建了便携配置，优先级高于 %APPDATA%。若不跟着换，监听线程会
         * 一直盯着旧文件，新配置的热加载永远不生效。 */
        if (wcscmp(g_watch_path, g_ini_path) == 0)
            return;
        HUA_LOG_I("配置路径已变化，重启监听");
        /* 旧线程没确认退出就不能重启：下面会 ResetEvent 停止事件并覆写 g_watch_path，
         * 那等于复活旧线程的等待条件 —— 两个线程同时读写 g_watch_path /
         * g_watch_fingerprint，还会重复投递重载。宁可维持现状。 */
        if (!watch_stop())
            return;
    }
    wcsncpy(g_watch_dir, g_ini_path, MAX_PATH - 1);
    g_watch_dir[MAX_PATH - 1] = L'\0';
    wcsncpy(g_watch_path, g_ini_path, MAX_PATH - 1);
    g_watch_path[MAX_PATH - 1] = L'\0';
    wchar_t *slash = wcsrchr(g_watch_dir, L'\\');
    if (!slash)
        return;
    slash[0] = L'\0';   /* 目录部分 */

    /* 手动重置事件；复用同一个句柄，重启监听时先复位。 */
    if (!g_watch_stop_evt) {
        g_watch_stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!g_watch_stop_evt) {
            HUA_LOG_W("创建监听停止事件失败: %lu（热加载不可用，请用托盘「重载配置」）",
                      GetLastError());
            return;
        }
    } else {
        ResetEvent(g_watch_stop_evt);
    }

    g_watch_thread = CreateThread(NULL, 0, watch_proc, NULL, 0, NULL);
    if (!g_watch_thread)
        HUA_LOG_W("启动配置监听线程失败: %lu（热加载不可用，请用托盘「重载配置」）",
                  GetLastError());
}

/* 停止监听线程。返回 true 表示已确认退出，可以安全重启；
 * false 表示线程还活着，调用方绝不能 ResetEvent 停止事件或覆写 g_watch_path。 */
static bool watch_stop(void)
{
    if (!g_watch_thread)
        return true;
    if (g_watch_stop_evt)
        SetEvent(g_watch_stop_evt);   /* 立刻叫醒；线程不再需要等满一轮 */
    /* 线程通常在毫秒级退出（等的是事件而非超时轮询），但读文件算指纹可能卡在
     * 慢盘/网络盘上。等待超时就不能当它已经死了：此前的写法照样 CloseHandle 并
     * 置 NULL，随后 watch_start 会 ResetEvent 停止事件，等于把旧线程的等待条件复活
     * → 双线程竞争 g_watch_path。 */
    if (WaitForSingleObject(g_watch_thread, 5000) != WAIT_OBJECT_0) {
        HUA_LOG_W("配置监听线程未能退出，维持现状（不重启监听）");
        return false;   /* 句柄故意不关：线程仍在用它自己的状态 */
    }
    CloseHandle(g_watch_thread);
    g_watch_thread = NULL;
    return true;
}

/* ---------------- 开机自启 ---------------- */

/* 把 ini 里的 AutoStart 行写回为 enable。
 *
 * 纯文本变换（分节、就地替换/插入/补节、嵌入 NUL 防护）已抽到 ini_edit.c 并单测；
 * 这里只剩「读文件 → 变换 → 写临时文件 → 原子替换」的 IO 外壳。原子替换只能防
 * 「写失败」，防不住「成功地写入了截断内容」，故变换那步在拿不准输入时会返回失败
 * （见 ini_set_autostart），绝不产出会毁掉用户配置的半截内容。 */
static void ini_write_autostart(bool enable)
{
    if (!g_ini_path[0])
        return;
    size_t len = 0;
    char *text = hua_read_file(g_ini_path, &len);
    if (!text)
        return;

    /* 变换可能变长（插入一行或补一整节）；给足余量，不足则 ini_set_autostart 返回
     * 失败而非截断。in_len*2 覆盖「每行都命中并被替换而变长」的病态上界。 */
    size_t cap = len * 2 + 64;
    char *out = malloc(cap);
    if (!out) {
        hua_free(text);
        HUA_LOG_W("写自启配置失败：内存不足（原配置未动）");
        return;
    }
    size_t n = ini_set_autostart(text, len, enable, out, cap);
    hua_free(text);
    if (n == HUA_INI_EDIT_FAIL) {
        free(out);
        HUA_LOG_W("拒绝改写自启开关：配置疑似含嵌入 NUL（UTF-16 编码）；"
                  "请将 hua.ini 另存为 UTF-8。自启开关未写回文件。");
        return;
    }

    /*
     * 构造临时路径。必须检查 _snwprintf 的返回值：它截断时返回 -1 且不写终止符，
     * 而 g_ini_path 最长可达 259 字符，此时拼上 ".tmp" 会被截回 259 —— 结果恰好
     * **等于 g_ini_path 本身**，下面的 _wfopen(tmp, "wb") 就会直接截断用户的实时
     * 配置，失败路径里的 _wremove(tmp) 更会把它整个删掉。
     */
    wchar_t tmp[MAX_PATH];
    int tn = _snwprintf(tmp, MAX_PATH, L"%s.tmp", g_ini_path);
    if (tn < 0 || tn >= MAX_PATH) {
        free(out);
        HUA_LOG_W("配置路径过长，无法构造临时文件（原配置未动）");
        return;
    }
    tmp[MAX_PATH - 1] = L'\0';

    FILE *fp = _wfopen(tmp, L"wb");
    if (!fp) {
        free(out);
        HUA_LOG_W("写自启配置失败：无法创建临时文件（原配置未动）");
        return;
    }

    bool ok = fwrite(out, 1, n, fp) == n;
    free(out);
    /* 缓冲数据要到 fclose 才真正落盘，这里的失败不能漏检。 */
    if (fclose(fp) != 0)
        ok = false;

    if (!ok) {
        _wremove(tmp);
        HUA_LOG_W("写自启配置失败（原配置未动）");
        return;
    }
    if (!MoveFileExW(tmp, g_ini_path, MOVEFILE_REPLACE_EXISTING)) {
        HUA_LOG_W("替换配置文件失败: %lu（原配置未动）", GetLastError());
        _wremove(tmp);
    }
}

/* 启动时把自启任务与配置对账。 */
static void autostart_reconcile(void)
{
    /*
     * 开启时**无条件重建**（RegisterTask 是 CREATE_OR_UPDATE，幂等）。
     * 不能只在「任务不存在」时才建：1.0.12 及更早版本建出来的任务带着一批错误的默认设置
     * （72 小时后被杀、电池下不启动、拔电源即停），而那些任务是"存在"的，只按存在与否
     * 对账的话，老用户永远迁移不过来。顺带也能自愈——任务被别的工具改坏了下次启动就修回。
     * 代价是每次启动多一次本地 RPC；这段本就排在装钩子之前，见 WinMain 处的顺序说明。
     *
     * 失败必须留痕：此前两个分支都只有 if 没有 else，对账失败是彻底静默的——
     * 用户看到的是「开机自启就是不工作」，日志里一个字都没有。
     * 成功日志由 autostart_set 自己打（它才知道具体建成了什么设置），这里只补失败后果。
     */
    if (g_config.auto_start) {
        if (!autostart_set(true))
            HUA_LOG_W("创建开机自启任务失败，开机不会自动启动（原因见上一条）");
    } else if (autostart_exists()) {
        if (!autostart_set(false))
            HUA_LOG_W("移除开机自启任务失败，开机仍会自动启动（原因见上一条）");
    }
}

/* ---------------- 托盘 ---------------- */

/* 释放托盘图标。仅 LoadImageW 得到的图标归我们所有；LoadIconW 取的系统预定义
 * 图标是共享的，销毁它是错误的（MSDN: Do not use DestroyIcon on a shared icon）。 */
static void tray_free_icon(void)
{
    if (g_nid.hIcon && g_icon_owned)
        DestroyIcon(g_nid.hIcon);
    g_nid.hIcon = NULL;
    g_icon_owned = false;
}

/* 返回是否成功注册托盘图标；调用方据此决定要不要挂补加计时器。 */
static bool tray_add(HWND hwnd)
{
    /* 本函数可能被 TaskbarCreated 重复调用。旧图标句柄要留到新注册建立之后再释放：
     * 若此刻就销毁，而下面的 NIM_ADD 又失败（例如广播是多余的、图标其实还在），
     * explorer 的旧注册仍引用着这个已销毁的 HICON，托盘图标会变成空白。 */
    HICON old_icon  = g_nid.hIcon;
    bool  old_owned = g_icon_owned;
    g_nid.hIcon = NULL;
    g_icon_owned = false;

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
    g_icon_owned = (g_nid.hIcon != NULL);   /* 非 LR_SHARED，需自行 DestroyIcon */
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);   /* 共享图标，不可销毁 */
    /* 悬浮提示三行：名称 / 简介 / 版本。szTip 为 128 WCHAR，\n 换行。 */
    wcscpy(g_nid.szTip, HUA_APP_NAME L"（划）\n"
                        HUA_DESCRIPTION L"\n"
                        L"v" HUA_VERSION);
    /* 先清掉可能残留的同 (hWnd,uID) 注册再添加。资源管理器重启后托盘区是新建的、
     * 旧记录本已随之消失，此调用会失败但无妨；而当广播是多余的（图标其实还在）时，
     * 不先删就会让 NIM_ADD 因重复而失败。 */
    Shell_NotifyIconW(NIM_DELETE, &g_nid);

    /* 托盘是本程序唯一的 UI 入口，加不上图标 = 用户无法重载配置也无法退出。
     * 最常见的失败（开机自启早于资源管理器建好托盘区）由 TaskbarCreated 兜底重建，
     * 这里至少要留下日志，否则完全不可诊断。 */
    bool ok = Shell_NotifyIconW(NIM_ADD, &g_nid) ? true : false;
    if (!ok)
        HUA_LOG_W("添加托盘图标失败: %lu（资源管理器尚未就绪；将定时重试并等待其广播）",
                  GetLastError());

    /* 新注册已建立（或已彻底失败），此时释放旧句柄才安全。 */
    if (old_icon && old_owned)
        DestroyIcon(old_icon);
    return ok;
}

static void tray_remove(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    tray_free_icon();
}

static void tray_show_menu(HWND hwnd)
{
    /* 初始化后再取：GetCursorPos 会失败（切到非输入桌面时），失败时用未初始化的
     * POINT 弹菜单等于读未初始化内存。项目其余三处 GetCursorPos 都判了返回值。 */
    POINT pt = {0};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_RELOAD,   L"重载配置");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_INI, L"打开配置文件");
    AppendMenuW(menu, MF_STRING | (g_config.auto_start ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_UPDATE,   L"检查更新");
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
    /* TaskbarCreated 的消息 ID 是运行时注册的，不能作为 case 标签，故在 switch 前拦。
     * 资源管理器重启会重建托盘区，此时必须重新 NIM_ADD，否则图标永久消失、
     * 用户再也无法重载配置或退出（本程序无其他 UI）。 */
    if (g_wm_taskbar_created && msg == g_wm_taskbar_created) {
        if (tray_add(hwnd)) {
            /* 广播来了且加成了，就不必再让补加计时器空转。 */
            KillTimer(hwnd, TIMER_TRAY_RETRY);
            HUA_LOG_I("收到 TaskbarCreated，已重建托盘图标");
        }
        return 0;
    }

    switch (msg) {
    case WM_CLOSE:
        /* 只经托盘「退出」退出。本窗口是顶层窗口（必须如此才能收到 TaskbarCreated），
         * 因而也会收到部分工具广播的 WM_CLOSE —— 交给 DefWindowProcW 就是 DestroyWindow
         * → 进程静默退出。message-only 窗口时代不收广播，故无此问题。 */
        return 0;

    case WM_HUA_TRAY:
        /* lParam 携带原始鼠标消息（未升级到 NOTIFYICON_VERSION_4）。 */
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            tray_show_menu(hwnd);
        }
        return 0;

    case WM_HUA_RELOAD:
        /* ini 文件变化：自动重载配置并按新触发键/阈值重装钩子。 */
        load_config();
        /* watch_start 在路径变化时会 watch_stop → WaitForSingleObject 阻塞最长 1s
         * 且不泵消息，超过 LowLevelHooksTimeout 就会摘掉钩子。必须排在装钩子之前
         * 跑完（同 WinMain 里 autostart_reconcile 的处理），否则弄死的正是刚装好的钩子。 */
        watch_start();   /* ini 路径可能已变，跟着换监听目标 */
        apply_hook_config();
        HUA_LOG_I("检测到 ini 变化，已自动重载");
        return 0;

    case WM_HUA_HOOK:
        if (wParam == HUA_EV_GESTURE_BEGIN) {
            /* 从 Down 开始轮询；移动超过阈值后浮层才会实际显示轨迹。 */
            g_active_has_exe = window_exe_lower(hook_last_target(),
                                                g_active_exe, sizeof(g_active_exe));
            overlay_begin();
            /* 失败必须留痕：这个定时器是手势的心跳——轮询光标、驱动浮层、跑停顿超时
             * 自愈。它没起来，手势就会卡在非 IDLE 态，而 TIMER_WATCHDOG 的重装恰恰
             * 被 hook_is_idle() 挡住 → 探活永久失效、手势整个会话静默死掉。
             * （TIMER_WATCHDOG 一直是判了返回值的，这里此前没判，不一致。） */
            if (!SetTimer(hwnd, TIMER_FRAME, 16, NULL))   /* ~60 FPS */
                HUA_LOG_E("启动手势帧计时器失败: %lu（本次手势将无轨迹且不会超时自愈）",
                          GetLastError());
        } else if (wParam == HUA_EV_GESTURE_CANCEL) {
            KillTimer(hwnd, TIMER_FRAME);
            overlay_end();
            hook_replay_trigger_click();
        } else if (wParam == HUA_EV_GESTURE_END) {
            /* 主线程执行：匹配 + 动作（不在钩子回调里做，避免 300ms 超时摘钩）。 */
            KillTimer(hwnd, TIMER_FRAME);

            const Pt *ended_pts = NULL;
            size_t ended_n = hook_snapshot(&ended_pts);
            const char *seq = hook_last_seq();
            HWND target = hook_last_target();
            /*
             * had_trail 要回答的是「用户到底划出东西了没有」——划出来了就认这是手势
             * （失败也算），没划出来就是一次普通点击，得把原生右键还回去。
             *
             * 「点数 >= 2」是个坏代理：状态机在 TriggerDistance(5px) 就进 Active，而
             * rec_encode 要到 MinDistance(20px) 才分出第一段。中间这段死区里只要采到
             * 2 个点（StepDistance=12px，手抖一下就够），had_trail 就为真 —— 于是
             * Down 和 Up 双双被吞，用户只是想点个右键，菜单却凭空消失，且「有没有菜单」
             * 取决于抖动幅度够不够采到第 2 个点，完全是随机的。
             * 真正的判据是「产出了方向段没有」：空串 = 什么都没划出来 = 普通点击。
             */
            bool had_trail = ended_n >= 2 && seq[0] != '\0';
            const char *action = config_resolve(&g_config,
                                                g_active_has_exe ? g_active_exe : NULL, seq);

            /* 用最终 seq 刷新一帧最终判定（命中动作名 / 「手势无动作」），再淡出。
             * 实时帧已在绘制过程中显示，这里只是收尾对齐末点。 */
            overlay_update(ended_pts, ended_n, seq, action ? action_label(action) : NULL);
            overlay_end();

            if (action) {
                /* action 是指向 g_config 的裸指针，而 action_execute 的 run: 分支会
                 * 调 ShellExecuteW —— 它会泵消息，可能在其内部重入派发 WM_HUA_RELOAD，
                 * 于是 load_config 把整个 g_config memset 掉。对象仍有效（静态数组）
                 * 故不是 UB，但之后的日志会打出陈旧/空的动作名。先拷一份再执行。 */
                char action_copy[CFG_MAX_ACTION];
                strncpy(action_copy, action, sizeof(action_copy) - 1);
                action_copy[sizeof(action_copy) - 1] = '\0';

                /* 日志必须在执行之后并带上结果：动作可能静默失败（如目标窗口
                 * 因前台锁定策略拒绝 SetForegroundWindow，一个键都没发出去）。
                 * 先打「→ key:ctrl+w」再执行，会让日志断言一件没发生的事，
                 * 用户报「手势偶尔失灵」时把排查引向完全错误的方向。 */
                bool ok = action_execute(action_copy, target);
                if (ok) {
                    HUA_LOG_I("手势 \"%s\" [%s] → %s", seq,
                              g_active_has_exe ? g_active_exe : "?", action_copy);
                } else {
                    HUA_LOG_W("手势 \"%s\" [%s] → %s 执行失败", seq,
                              g_active_has_exe ? g_active_exe : "?", action_copy);
                }
            } else {
                HUA_LOG_I("手势 \"%s\" → 未匹配%s", seq,
                          had_trail ? "（已有轨迹，不补发右键）" : "");
                /*
                 * 划出了轨迹却没匹配上：把原始采样点原样打出来。
                 * 方向串本身不足以复现问题——它是 rec_encode 的产物，看到 "26969696"
                 * 也无从知道用户到底画了什么、编码在哪一步跑偏。没有原始点，就只能拿
                 * 合成轨迹猜真实输入，而合成参数（采样密度、手势尺寸）一旦与实际不符，
                 * 得到的"验证通过"是假的。这行日志就是为了不再靠猜。
                 */
                if (had_trail && ended_pts) {
                    char pbuf[900];
                    size_t used = 0;
                    for (size_t i = 0; i < ended_n && used + 24 < sizeof(pbuf); i++) {
                        int w = snprintf(pbuf + used, sizeof(pbuf) - used, "%s%d,%d",
                                         i ? " " : "", ended_pts[i].x, ended_pts[i].y);
                        if (w < 0)
                            break;
                        used += (size_t)w;
                    }
                    pbuf[used] = '\0';
                    HUA_LOG_I("  未匹配轨迹原始点：n=%zu min_dist=%d step=%d [%s]",
                              ended_n, g_config.min_distance, g_config.step_distance, pbuf);
                }
                /* 只有从未形成可见轨迹时才允许还原；长轨迹失败也属于手势。 */
                if (g_config.restore_event && !had_trail)
                    hook_replay_trigger_click();
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_TRAY_RETRY) {
            /* 开机时资源管理器还没建好托盘区。这里定时补加，不依赖 TaskbarCreated 送达。 */
            static int attempts;
            if (tray_add(hwnd)) {
                KillTimer(hwnd, TIMER_TRAY_RETRY);
                HUA_LOG_I("托盘图标已补加成功（第 %d 次重试）", attempts + 1);
                attempts = 0;
            } else if (++attempts >= TRAY_RETRY_MAX_ATTEMPTS) {
                KillTimer(hwnd, TIMER_TRAY_RETRY);
                HUA_LOG_E("托盘图标补加失败：重试 %d 次仍未成功，放弃（手势仍可用，"
                          "但无法从托盘重载配置或退出）", attempts);
                attempts = 0;
            }
            return 0;
        }
        if (wParam == TIMER_WATCHDOG) {
            /* 钩子被系统静默摘掉后 g_hook 仍非空，hook_install 会误判「已装好」，
             * 手势将永久失效且用户无从察觉。故探活到异常就强制重装。
             * 手势进行中不重装：重装会清掉 g_suppress_trigger_up，导致用户松手时
             * 孤儿 Up 泄漏给目标程序（弹出右键菜单）。下个周期再处理即可。 */
            if (hook_looks_dead() && hook_is_idle()) {
                /* 日志限频：hua_logf 会 fflush，若日志落在慢盘上，单次 fflush 超过
                 * LowLevelHooksTimeout 就会把刚装好的钩子再弄死 → 下周期又重装 →
                 * 又打日志，形成自维持的重装循环。 */
                ULONGLONG now = GetTickCount64();
                if (now - g_last_revive_log >= WATCHDOG_LOG_THROTTLE_MS) {
                    g_last_revive_log = now;
                    HUA_LOG_W("检测到钩子未生效（疑似回调超时被系统摘掉），正在重装");
                }
                apply_hook_config();
            }
            return 0;
        }
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
                char partial_seq[REC_MAX_SEQ];
                rec_encode(timed_out_pts, timed_out_n, g_config.min_distance,
                           partial_seq, sizeof(partial_seq));

                /*
                 * 空串 = 一个方向段都没划出来 = 用户只是按住触发键不动想了一下。
                 * 手抖 TriggerDistance(5px) 就足以把状态推进 ST_ACTIVE，于是 Down 被吞、
                 * Up 也被吞，这次右键凭空消失、菜单永远不弹（已在实机日志中复现：
                 * 位移 5.4px、点数 1，照样判超时并吞掉右键）。
                 * 与 END 路径同一条规则：没划出方向段就还原原生点击，是否还原同样交给
                 * RestoreEvent。真划出了东西再超时（如画了「2」然后发呆），仍按手势处理、
                 * 不补发。
                 */
                bool drew_nothing = (partial_seq[0] == '\0');
                if (drew_nothing && g_config.restore_event)
                    hook_arm_replay_on_up();

                char target_exe[CFG_MAX_EXE], down_exe[CFG_MAX_EXE];
                char current_exe[CFG_MAX_EXE];
                /*
                 * 窗口标题只在 Debug 构建记录。日志沿用「便携优先」写在 exe 同级，
                 * 该目录对其他用户是可读的（Users:RX），而窗口标题常含文档名、URL、
                 * 邮件主题——管理员会话的标题被普通用户读走属跨用户信息泄露。
                 * exe 名与窗口句柄的诊断价值相近但敏感度低得多，故 Release 保留它们。
                 */
#ifndef NDEBUG
                char target_title[512], current_title[512];
#else
                const char *target_title = "(仅 Debug 记录)";
                const char *current_title = "(仅 Debug 记录)";
#endif
                bool has_target_exe = window_exe_lower(timeout_info.target,
                                                       target_exe, sizeof(target_exe));
                bool has_down_exe = window_exe_lower(timeout_info.foreground_at_down,
                                                     down_exe, sizeof(down_exe));
                bool has_current_exe = window_exe_lower(timeout_info.foreground_at_timeout,
                                                        current_exe, sizeof(current_exe));
#ifndef NDEBUG
                window_title_utf8(timeout_info.target,
                                  target_title, sizeof(target_title));
                window_title_utf8(timeout_info.foreground_at_timeout,
                                  current_title, sizeof(current_title));
#endif

                HUA_LOG_I("手势停顿超时：idle=%llums 达到 PauseTimeout=%lums，已取消；%s；trigger=%s，轨迹=%s，点数=%zu，起点=(%d,%d)，末点=(%d,%d)，当前光标=(%d,%d)",
                          (unsigned long long)timeout_info.idle_ms,
                          (unsigned long)timeout_info.timeout_ms,
                          drew_nothing
                              ? (g_config.restore_event
                                     ? "未划出方向段，视同普通点击，将在松手时补发触发键"
                                     : "未划出方向段，但 RestoreEvent=false，不补发触发键")
                              : "已划出方向段，按手势处理，不补发触发键",
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
            char seq[REC_MAX_SEQ];
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
            watch_start();   /* 同上：可能阻塞，必须在装钩子之前 */
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
                /* 原文案断言「需要管理员权限」——本程序 manifest 已是
                 * requireAdministrator，权限恰恰是最不可能的原因，那句话只会把用户
                 * 的排查引向死胡同。真实原因（schtasks 退出码/错误码）已由
                 * autostart.c 记进日志，这里如实指过去。 */
                MessageBoxW(hwnd,
                            L"设置开机自启失败。\n\n"
                            L"常见原因：任务计划程序服务被禁用，或被组策略/安全软件拦截。\n"
                            L"详细错误码见 hua.log。",
                            HUA_APP_NAME, MB_OK | MB_ICONWARNING);
            }
            break;
        }
        case IDM_UPDATE:
            update_check_now(hwnd, &g_config);
            break;
        case IDM_PROJECT:
            ShellExecuteW(NULL, L"open", HUA_PROJECT_URL, NULL, NULL, SW_SHOWNORMAL);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_HUA_UPDATE:
        update_on_found_message(hwnd);
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

    /* 若本次是自动更新后的重启：先等旧进程退出、清理 .old 残留，【必须早于下面的
     * CreateMutexW】。否则旧进程尚未释放互斥量，新进程会误判「已在运行」而退出，
     * 结果一个实例都不剩。见 update.c / updater 库的单实例握手说明。 */
    update_boot();

    /* 单实例：命名互斥量。已存在则前置提示并退出。 */
    HANDLE mutex = CreateMutexW(NULL, TRUE, HUA_MUTEX_NAME);
    if (!mutex) {
        /* 创建失败（同名不同类型的内核对象占位 / ACL 拒绝）会让单实例保护静默失效，
         * 跑起两个实例、两套钩子互相吞事件。此时 hua_log_init 尚未调用，只能弹窗。 */
        MessageBoxW(NULL, L"无法创建单实例互斥量，hua 无法启动。",
                    HUA_APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"hua 已在运行。", HUA_APP_NAME, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        return 0;
    }

    configure_log_before_init();
    hua_log_init();

    /* 注册并创建不可见的顶层窗口（无可见 UI，但能跑消息循环 + 收托盘/钩子/广播消息）。 */
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

    hook_set_gate(gesture_gate);
    load_config();

    /*
     * 顺序要紧：autostart_reconcile 会同步调任务计划的 COM 接口（每次启动至少一次
     * autostart_exists），期间不泵消息。改用 COM 后不再 spawn schtasks，耗时已大幅下降，
     * 但仍是一次跨进程 RPC——开机时 Task Scheduler 服务可能正在启动，反而更慢。
     * 故必须把它排在**创建窗口与装钩子之前**，有两个独立的理由：
     *   1) LL 钩子回调必须靠本线程检索消息才能派发，超过 LowLevelHooksTimeout
     *      （默认 300ms）系统就静默摘钩且不通知。若这段 RPC 卡在装钩子之后，钩子会在
     *      消息循环启动前就被摘掉，表现为托盘正常、日志「就绪」、但手势整个会话无响应。
     *   2) 我们的窗口是顶层窗口（见下），一旦创建就是广播目标。此时若卡在 RPC 上不泵
     *      消息，别的进程 SendMessageTimeout(HWND_BROADCAST, ...)（如安装器改 PATH 后
     *      广播 WM_SETTINGCHANGE）会被我们拖到超时。
     */
    autostart_reconcile();

    /*
     * 这里**不能**用 message-only 窗口（HWND_MESSAGE）：MSDN 明定 message-only
     * 窗口不接收广播消息，而资源管理器重启后的 TaskbarCreated 是经 HWND_BROADCAST
     * 只投递给顶层窗口的——用 HWND_MESSAGE 会导致托盘图标永远无法重建，而托盘是
     * 本程序唯一的 UI 入口（图标丢了就无法重载配置、无法退出）。
     * 故改用「从不显示的顶层窗口」：父窗口为 NULL 即顶层，能收广播；WS_EX_TOOLWINDOW
     * 使其不出现在 Alt-Tab 与任务栏；不调 ShowWindow 则始终不可见。
     * 已实测：message-only 收不到广播，本写法能收到。
     */
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, HUA_WINDOW_CLASS, HUA_APP_NAME,
                             WS_POPUP, 0, 0, 0, 0,
                             NULL, NULL, hInstance, NULL);
    if (!g_hwnd) {
        HUA_LOG_E("CreateWindowExW 失败: %lu", GetLastError());
        goto cleanup_mutex;
    }

    /* 必须在 tray_add 之前注册：资源管理器重启后靠它重建图标。 */
    g_wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (!g_wm_taskbar_created) {
        HUA_LOG_W("注册 TaskbarCreated 失败: %lu（资源管理器重启后托盘图标不会恢复）",
                  GetLastError());
    } else {
        /*
         * 放行 UIPI，否则这条广播根本送不到本进程。
         *
         * 本程序以管理员权限运行（清单 requireAdministrator + 任务计划 HighestAvailable），
         * 完整性级别高于广播方 explorer.exe；而 UIPI 只允许消息从【同级或更高】流向更低，
         * 低→高一律丢弃且不报错。实测过一台机器：hua 为 System(12288)、explorer 为
         * High(8192)，开机时 NIM_ADD 失败后再也收不到 TaskbarCreated，托盘图标整个会话都
         * 不出现（进程却活着，手势正常）——正是这条过滤把兜底路径本身给挡了。
         * ChangeWindowMessageFilterEx 把该消息按窗口加白名单，是微软给提权程序的标准解法。
         */
        if (!ChangeWindowMessageFilterEx(g_hwnd, g_wm_taskbar_created, MSGFLT_ALLOW, NULL))
            HUA_LOG_W("放行 TaskbarCreated 失败: %lu（提权运行时可能收不到该广播）",
                      GetLastError());
    }

    /* 加不上就挂计时器补加：不能只依赖广播（见上）。 */
    if (!tray_add(g_hwnd)) {
        if (!SetTimer(g_hwnd, TIMER_TRAY_RETRY, TRAY_RETRY_INTERVAL_MS, NULL))
            HUA_LOG_E("托盘补加计时器创建失败: %lu（托盘图标可能整个会话都不出现）",
                      GetLastError());
    }

    if (!overlay_init(hInstance))
        HUA_LOG_W("overlay 初始化失败（浮层不可用，不影响手势）");

    apply_hook_config();
    watch_start();
    /* 钩子可能被系统静默摘掉（回调超时），且无任何通知 → 定期探活重装。 */
    if (!SetTimer(g_hwnd, TIMER_WATCHDOG, WATCHDOG_INTERVAL_MS, NULL))
        HUA_LOG_W("启动钩子探活计时器失败: %lu（钩子失效后将无法自动恢复）",
                  GetLastError());
    HUA_LOG_I("hua 就绪（识别闭环 + per-app 门控 + 浮层 + 热加载）");

    /* 启动时后台静默检查更新（受 [Update] Enabled/AutoCheck 控制）。发现新版仅弹气泡。 */
    update_start_background_check(g_hwnd, &g_config);

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

cleanup_mutex:
    /* 日志收尾必须放在这里：早退路径（RegisterClassExW / CreateWindowExW 失败）
     * 也要走到——恰恰是最需要诊断的那条路径上，若跳过它，日志尾行与 fclose 的
     * flush 都不会发生，错误信息可能还留在缓冲里就随进程消失了。 */
    hua_log_close();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
