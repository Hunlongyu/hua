/*
 * platform.c —— 见 platform.h。
 */
#include "platform.h"

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

static const char *level_str(HuaLogLevel level)
{
    switch (level) {
    case HUA_LOG_WARN:  return "WARN";
    case HUA_LOG_ERROR: return "ERROR";
    case HUA_LOG_INFO:
    default:            return "INFO";
    }
}

void hua_log_init(void)
{
    if (g_log_ready)
        return;

    InitializeCriticalSection(&g_log_cs);

    /* 路径优先级：exe 同级目录（便携）优先；不可写则退回 %APPDATA%\hua\。 */
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];

    /* 二进制追加：直接写 UTF-8 字节（源码/字面量已是 UTF-8），不走 CRT 的
     * Unicode 翻译层——ccs=UTF-8 会把流设成宽字符模式，narrow fprintf 会失效。
     * 失败也不致命，后续 hua_logf 会静默跳过。 */
    /* _snwprintf 截断时返回 -1 且不写终止符，必须手动补（与 hua_appdata_dir 一致）。 */
    if (hua_exe_dir(dir, MAX_PATH)) {
        _snwprintf(path, MAX_PATH, L"%shua.log", dir);
        path[MAX_PATH - 1] = L'\0';
        g_log_fp = _wfopen(path, L"ab");
    }
    if (!g_log_fp && hua_appdata_dir(dir, MAX_PATH)) {
        _snwprintf(path, MAX_PATH, L"%shua.log", dir);
        path[MAX_PATH - 1] = L'\0';
        g_log_fp = _wfopen(path, L"ab");
    }
    if (g_log_fp) {
        fseek(g_log_fp, 0, SEEK_END);
        if (ftell(g_log_fp) == 0) {
            /* 新文件：写 UTF-8 BOM，方便 Notepad 正确识别中文。 */
            static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            fwrite(bom, 1, sizeof(bom), g_log_fp);
        }
    }
    g_log_ready = true;

    HUA_LOG_I("==== hua 启动 ====");
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
    if (!g_log_ready || !g_log_fp)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_cs);
    if (g_log_fp) {
        fprintf(g_log_fp, "%02d:%02d:%02d.%03d [%s] %s\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                level_str(level), msg);
        fflush(g_log_fp);
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

    /* 先确保 %APPDATA%\hua 存在（不含末尾反斜杠给 CreateDirectory）。 */
    wchar_t dir[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%s\\hua", base);
    dir[MAX_PATH - 1] = L'\0';
    CreateDirectoryW(dir, NULL);   /* 已存在也无妨 */

    _snwprintf(out, cap, L"%s\\hua\\", base);
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

char *hua_read_file(const wchar_t *path)
{
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
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    buf[rd] = '\0';
    fclose(fp);
    return buf;
}
