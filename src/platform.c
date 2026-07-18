/*
 * platform.c —— 见 platform.h。
 */
#include "platform.h"

#include "version.h"

/* INITGUID 让 FOLDERID_* 的 GUID 在本 TU 内实体化，免去链接 uuid.lib。 */
#define INITGUID
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ---------------- 日志 ---------------- */

static FILE            *g_log_fp;
static CRITICAL_SECTION g_log_cs;
static bool             g_log_ready;
static HuaLogLevel      g_log_min_level = HUA_LOG_WARN;
static unsigned long long g_log_max_bytes = 10ULL * 1024ULL * 1024ULL;
static int              g_log_retention_days = 2;
static unsigned long long g_log_bytes;
static wchar_t          g_log_dir[MAX_PATH];
static wchar_t          g_log_path[MAX_PATH];

static const char *level_str(HuaLogLevel level)
{
    switch (level) {
    case HUA_LOG_WARN:  return "WARN";
    case HUA_LOG_ERROR: return "ERROR";
    case HUA_LOG_INFO:
    default:            return "INFO";
    }
}

static void write_log_bom_locked(void)
{
    if (!g_log_fp)
        return;
    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    if (fwrite(bom, 1, sizeof(bom), g_log_fp) == sizeof(bom))
        g_log_bytes = sizeof(bom);
}

static bool open_log_in_dir_locked(const wchar_t *dir)
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%shua.log", dir);
    path[MAX_PATH - 1] = L'\0';

    FILE *fp = _wfopen(path, L"ab");
    if (!fp)
        return false;

    g_log_fp = fp;
    wcsncpy(g_log_dir, dir, MAX_PATH - 1);
    g_log_dir[MAX_PATH - 1] = L'\0';
    wcsncpy(g_log_path, path, MAX_PATH - 1);
    g_log_path[MAX_PATH - 1] = L'\0';

    _fseeki64(g_log_fp, 0, SEEK_END);
    __int64 pos = _ftelli64(g_log_fp);
    g_log_bytes = pos > 0 ? (unsigned long long)pos : 0;
    if (g_log_bytes == 0)
        write_log_bom_locked();
    return true;
}

static bool open_log_locked(void)
{
    wchar_t dir[MAX_PATH];
    if (hua_exe_dir(dir, MAX_PATH) && open_log_in_dir_locked(dir))
        return true;
    if (hua_appdata_dir(dir, MAX_PATH) && open_log_in_dir_locked(dir))
        return true;
    return false;
}

/* 删除形如 hua-YYYYMMDD-HHMMSS-mmm.log 的过期轮转文件；当前 hua.log 永不误删。 */
static void cleanup_old_logs_locked(void)
{
    if (!g_log_dir[0] || g_log_retention_days <= 0)
        return;

    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%shua-*.log", g_log_dir);
    pattern[MAX_PATH - 1] = L'\0';

    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now;
    now.LowPart = now_ft.dwLowDateTime;
    now.HighPart = now_ft.dwHighDateTime;
    const unsigned long long day_ticks = 24ULL * 60ULL * 60ULL * 10000000ULL;
    const unsigned long long max_age = (unsigned long long)g_log_retention_days * day_ticks;

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        ULARGE_INTEGER modified;
        modified.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        modified.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (now.QuadPart > modified.QuadPart && now.QuadPart - modified.QuadPart > max_age) {
            wchar_t path[MAX_PATH];
            _snwprintf(path, MAX_PATH, L"%s%s", g_log_dir, fd.cFileName);
            path[MAX_PATH - 1] = L'\0';
            DeleteFileW(path);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static bool rotate_log_locked(const SYSTEMTIME *st)
{
    if (!g_log_fp || !g_log_path[0] || !g_log_dir[0])
        return false;

    fclose(g_log_fp);
    g_log_fp = NULL;

    wchar_t archived[MAX_PATH];
    _snwprintf(archived, MAX_PATH,
               L"%shua-%04u%02u%02u-%02u%02u%02u-%03u.log", g_log_dir,
               st->wYear, st->wMonth, st->wDay,
               st->wHour, st->wMinute, st->wSecond, st->wMilliseconds);
    archived[MAX_PATH - 1] = L'\0';

    if (!MoveFileExW(g_log_path, archived, MOVEFILE_REPLACE_EXISTING)) {
        g_log_fp = _wfopen(g_log_path, L"ab");
        if (g_log_fp) {
            _fseeki64(g_log_fp, 0, SEEK_END);
            __int64 pos = _ftelli64(g_log_fp);
            g_log_bytes = pos > 0 ? (unsigned long long)pos : 0;
        }
        return false;
    }

    g_log_fp = _wfopen(g_log_path, L"wb");
    g_log_bytes = 0;
    if (g_log_fp)
        write_log_bom_locked();
    cleanup_old_logs_locked();
    return g_log_fp != NULL;
}

void hua_log_configure(HuaLogLevel min_level, int max_size_mb, int retention_days)
{
    if (min_level < HUA_LOG_INFO || min_level > HUA_LOG_OFF)
        min_level = HUA_LOG_WARN;
    if (max_size_mb < 1)
        max_size_mb = 10;
    if (retention_days < 1)
        retention_days = 2;

    if (!g_log_ready) {
        g_log_min_level = min_level;
        g_log_max_bytes = (unsigned long long)max_size_mb * 1024ULL * 1024ULL;
        g_log_retention_days = retention_days;
        return;
    }

    EnterCriticalSection(&g_log_cs);
    g_log_min_level = min_level;
    g_log_max_bytes = (unsigned long long)max_size_mb * 1024ULL * 1024ULL;
    g_log_retention_days = retention_days;

    if (g_log_min_level == HUA_LOG_OFF) {
        if (g_log_fp) {
            fclose(g_log_fp);
            g_log_fp = NULL;
        }
    } else {
        if (!g_log_fp)
            open_log_locked();
        if (g_log_fp && g_log_bytes >= g_log_max_bytes) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            rotate_log_locked(&st);
        }
        cleanup_old_logs_locked();
    }
    LeaveCriticalSection(&g_log_cs);
}

void hua_log_init(void)
{
    if (g_log_ready)
        return;

    InitializeCriticalSection(&g_log_cs);

    g_log_ready = true;
    if (g_log_min_level != HUA_LOG_OFF) {
        EnterCriticalSection(&g_log_cs);
        if (open_log_locked()) {
            if (g_log_bytes >= g_log_max_bytes) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                rotate_log_locked(&st);
            }
            cleanup_old_logs_locked();
        }
        LeaveCriticalSection(&g_log_cs);
    }

    /* 版本号必须打在启动行：它是用户判断「跑的是不是刚构建的 exe」的唯一凭据。
     * 构建时 exe 若被运行中的实例锁住，链接会失败（LNK1104）而产物停在旧版本，
     * 没有这个标记的话，接下来测出的所有行为都会被归咎于新改的代码。 */
    HUA_LOG_I("==== hua %s 启动 ====", HUA_VERSION_STR);
}

void hua_log_close(void)
{
    if (!g_log_ready)
        return;
    HUA_LOG_I("==== hua 退出 ====");
    EnterCriticalSection(&g_log_cs);
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    LeaveCriticalSection(&g_log_cs);
    DeleteCriticalSection(&g_log_cs);
    g_log_ready = false;
}

void hua_logf(HuaLogLevel level, const char *fmt, ...)
{
    if (!g_log_ready)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[1280];
    int line_len = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\r\n",
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                            level_str(level), msg);
    if (line_len <= 0)
        return;
    size_t write_len = (size_t)line_len < sizeof(line) ? (size_t)line_len : sizeof(line) - 1;

    EnterCriticalSection(&g_log_cs);
    if (g_log_fp && level >= g_log_min_level && g_log_min_level != HUA_LOG_OFF) {
        if (g_log_bytes + write_len > g_log_max_bytes && g_log_bytes > 3)
            rotate_log_locked(&st);
        if (g_log_fp) {
            size_t written = fwrite(line, 1, write_len, g_log_fp);
            g_log_bytes += written;
            fflush(g_log_fp);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

/* ---------------- 编码转换 ---------------- */

wchar_t *hua_utf8_to_utf16(const char *s)
{
    if (!s)
        return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *out = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!out)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

char *hua_utf16_to_utf8(const wchar_t *s)
{
    if (!s)
        return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *out = (char *)malloc((size_t)n);
    if (!out)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

void hua_free(void *p)
{
    free(p);
}

/* ---------------- 路径 ---------------- */

bool hua_exe_dir(wchar_t *out, size_t cap)
{
    if (!out || cap == 0)
        return false;
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap)
        return false;
    /* 截到最后一个反斜杠（含）。 */
    wchar_t *slash = wcsrchr(out, L'\\');
    if (!slash)
        return false;
    slash[1] = L'\0';
    return true;
}

bool hua_appdata_dir(wchar_t *out, size_t cap)
{
    if (!out || cap == 0)
        return false;

    /*
     * 用 SHGetKnownFolderPath 而非 %APPDATA% 环境变量：本程序经 UAC 提权启动时会
     * 继承请求方的环境块，中完整性的攻击者可用改过 APPDATA 的环境拉起 hua，把配置
     * 与日志重定向到自己控制的目录——而配置里的 run: 动作是以管理员权限执行的。
     * KnownFolder 走的是注册表/令牌，不受环境变量摆布。
     */
    wchar_t base[MAX_PATH];
    PWSTR known = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &known)) || !known) {
        if (known)
            CoTaskMemFree(known);
        return false;
    }
    size_t klen = wcslen(known);
    if (klen == 0 || klen >= MAX_PATH) {
        CoTaskMemFree(known);
        return false;
    }
    wcsncpy(base, known, MAX_PATH - 1);
    base[MAX_PATH - 1] = L'\0';
    CoTaskMemFree(known);

    /* 先确保 %APPDATA%\hua 存在（不含末尾反斜杠给 CreateDirectory）。
     *
     * 两处拼接都必须判截断，不能只补终止符了事：klen 最大可到 259，加上 "\hua\"
     * 就会溢出 MAX_PATH。此前的写法补完 NUL 就 return true，等于把一个**被截断的
     * 路径当成功交出去**——klen=258 时 out 成了 AppData 根目录，配置被写到
     * %APPDATA%\hua.ini；klen=255 时尾反斜杠被截掉，调用方会拼出 %APPDATA%\huahua.ini
     * 这种垃圾文件名。同文件的 hua_exe_dir 与 main.c 的 .tmp 拼接都严格判了返回值，
     * 这里理应一致。 */
    wchar_t dir[MAX_PATH];
    int dn = _snwprintf(dir, MAX_PATH, L"%s\\hua", base);
    if (dn < 0 || dn >= MAX_PATH)
        return false;
    dir[MAX_PATH - 1] = L'\0';
    CreateDirectoryW(dir, NULL);   /* 已存在也无妨 */

    int on = _snwprintf(out, cap, L"%s\\hua\\", base);
    if (on < 0 || (size_t)on >= cap)
        return false;   /* 截断即失败：调用方有兜底（日志放弃 / 配置落内置默认） */
    out[cap - 1] = L'\0';
    return true;
}

bool hua_file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool hua_write_file(const wchar_t *path, const void *data, size_t len)
{
    FILE *fp = _wfopen(path, L"wb");
    if (!fp)
        return false;
    size_t wr = fwrite(data, 1, len, fp);
    /* 缓冲数据要到 fclose 才真正落盘（磁盘满等失败在这里才暴露），
     * 漏检它会把失败误报成成功。 */
    bool closed = (fclose(fp) == 0);
    return wr == len && closed;
}

char *hua_read_file(const wchar_t *path, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    FILE *fp = _wfopen(path, L"rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    /* 用实际读取数而非 sz 终止：短读（文件被并发截断 / IO 错误）时不暴露未初始化字节。 */
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    buf[rd] = '\0';
    fclose(fp);
    if (out_len)
        *out_len = rd;
    return buf;
}
