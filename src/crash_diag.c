#include "crash_diag.h"
#include "version.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <shlobj.h>

static wchar_t g_directory[MAX_PATH];
static wchar_t g_marker_path[MAX_PATH];
static char g_previous_run[256];
static bool g_initialized;
static bool g_clean;

typedef enum {
    OLD_MARKER_ABSENT,
    OLD_MARKER_PRESENT,
    OLD_MARKER_ERROR
} OldMarkerReadResult;

static bool make_directory(const wchar_t *directory)
{
    if (CreateDirectoryW(directory, NULL))
        return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    DWORD attributes = GetFileAttributesW(directory);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool join_path(wchar_t *path, size_t cap, const wchar_t *directory,
                      const wchar_t *name)
{
    int written = _snwprintf(path, cap, L"%s%s", directory, name);
    if (written < 0 || (size_t)written >= cap) {
        path[cap - 1] = L'\0';
        return false;
    }
    return true;
}

static void reset_state(void)
{
    g_directory[0] = L'\0';
    g_marker_path[0] = L'\0';
    g_previous_run[0] = '\0';
    g_initialized = false;
    g_clean = false;
}

static OldMarkerReadResult read_old_marker(char *text, size_t cap, size_t *length)
{
    HANDLE file = CreateFileW(g_marker_path, GENERIC_READ, FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return OLD_MARKER_ABSENT;
        return OLD_MARKER_ERROR;
    }

    DWORD read = 0;
    BOOL read_ok = ReadFile(file, text, (DWORD)(cap - 1), &read, NULL);
    BOOL close_ok = CloseHandle(file);
    if (!read_ok || !close_ok)
        return OLD_MARKER_ERROR;

    text[read] = '\0';
    *length = read;
    return OLD_MARKER_PRESENT;
}

static bool copy_marker_value(const char *line, const char *key,
                              char *value, size_t cap)
{
    size_t key_length = strlen(key);
    if (strncmp(line, key, key_length) != 0)
        return false;

    const char *source = line + key_length;
    size_t length = strlen(source);
    if (length == 0 || length >= cap)
        return false;
    memcpy(value, source, length + 1);
    return true;
}

static bool parse_previous_marker(char *text, size_t length)
{
    char version[64] = "";
    char started[64] = "";
    unsigned long pid = 0;
    bool have_version = false;
    bool have_pid = false;
    bool have_started = false;

    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20 && ch != '\r' && ch != '\n')
            return false;
    }

    char *line = text;
    char *end = text + length;
    while (line < end) {
        char *separator = line;
        while (separator < end && *separator != '\r' && *separator != '\n')
            ++separator;
        if (separator < end) {
            if (*separator == '\r') {
                if (separator + 1 >= end || separator[1] != '\n')
                    return false;
                *separator = '\0';
                line = separator + 2;
            } else {
                *separator = '\0';
                line = separator + 1;
            }
        } else {
            line = end;
        }

        char *current = text;
        if (strncmp(current, "version=", 8) == 0) {
            if (have_version || !copy_marker_value(current, "version=", version,
                                                   sizeof(version)))
                return false;
            have_version = true;
        } else if (strncmp(current, "pid=", 4) == 0) {
            if (have_pid || current[4] == '\0')
                return false;
            unsigned long parsed = 0;
            for (const char *digit = current + 4; *digit; ++digit) {
                if (*digit < '0' || *digit > '9' ||
                    parsed > (ULONG_MAX - (unsigned long)(*digit - '0')) / 10)
                    return false;
                parsed = parsed * 10 + (unsigned long)(*digit - '0');
            }
            pid = parsed;
            have_pid = true;
        } else if (strncmp(current, "started=", 8) == 0) {
            if (have_started || !copy_marker_value(current, "started=", started,
                                                   sizeof(started)))
                return false;
            have_started = true;
        }

        text = line;
    }

    if (!have_version || !have_pid || !have_started)
        return false;

    int written = snprintf(g_previous_run, sizeof(g_previous_run),
                           "version=%s pid=%lu started=%s", version, pid, started);
    if (written <= 0 || (size_t)written >= sizeof(g_previous_run)) {
        g_previous_run[0] = '\0';
        return false;
    }
    return true;
}

static bool write_marker(void)
{
    SYSTEMTIME local_time;
    GetLocalTime(&local_time);

    TIME_ZONE_INFORMATION timezone;
    DWORD timezone_id = GetTimeZoneInformation(&timezone);
    LONG bias = timezone.Bias;
    if (timezone_id == TIME_ZONE_ID_STANDARD)
        bias += timezone.StandardBias;
    else if (timezone_id == TIME_ZONE_ID_DAYLIGHT)
        bias += timezone.DaylightBias;

    int offset_minutes = -(int)bias;
    char iso_timestamp[48];
    int timestamp_len = snprintf(iso_timestamp, sizeof(iso_timestamp),
                                 "%04u-%02u-%02uT%02u:%02u:%02u.%03u%c%02d:%02d",
                                 local_time.wYear, local_time.wMonth, local_time.wDay,
                                 local_time.wHour, local_time.wMinute, local_time.wSecond,
                                 local_time.wMilliseconds,
                                 offset_minutes < 0 ? '-' : '+',
                                 abs(offset_minutes) / 60, abs(offset_minutes) % 60);
    if (timestamp_len <= 0 || (size_t)timestamp_len >= sizeof(iso_timestamp))
        return false;

    char marker_text[256];
    int marker_len = snprintf(marker_text, sizeof(marker_text),
        "format=1\r\nversion=%s\r\npid=%lu\r\nstarted=%s\r\n",
        HUA_VERSION_STR, (unsigned long)GetCurrentProcessId(), iso_timestamp);
    if (marker_len <= 0 || (size_t)marker_len >= sizeof(marker_text))
        return false;

    wchar_t temp_path[MAX_PATH];
    if (!join_path(temp_path, MAX_PATH, g_directory, L"hua-running.state.tmp"))
        return false;

    HANDLE file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    BOOL write_ok = WriteFile(file, marker_text, (DWORD)marker_len, &written, NULL);
    BOOL close_ok = CloseHandle(file);
    if (!write_ok || written != (DWORD)marker_len || !close_ok) {
        DeleteFileW(temp_path);
        return false;
    }

    if (!MoveFileExW(temp_path, g_marker_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp_path);
        return false;
    }
    return true;
}

static bool init_in_directory(const wchar_t *directory)
{
    reset_state();
    if (!directory || directory[0] == L'\0')
        return false;

    size_t length = wcslen(directory);
    if (length >= MAX_PATH - 1)
        return false;
    wcsncpy(g_directory, directory, MAX_PATH - 1);
    g_directory[MAX_PATH - 1] = L'\0';
    length = wcslen(g_directory);
    if (length > 0 && g_directory[length - 1] != L'\\') {
        g_directory[length] = L'\\';
        g_directory[length + 1] = L'\0';
    }

    if (!make_directory(g_directory) ||
        !join_path(g_marker_path, MAX_PATH, g_directory, L"hua-running.state")) {
        reset_state();
        return false;
    }

    char old_marker[512];
    size_t old_length = 0;
    OldMarkerReadResult old_marker_result =
        read_old_marker(old_marker, sizeof(old_marker), &old_length);
    if (old_marker_result == OLD_MARKER_ERROR) {
        reset_state();
        return false;
    }
    if (old_marker_result == OLD_MARKER_PRESENT)
        parse_previous_marker(old_marker, old_length);

    if (!write_marker()) {
        reset_state();
        return false;
    }

    g_initialized = true;
    return true;
}

bool crash_diag_init(void)
{
    wchar_t exe_path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    wchar_t *filename = wcsrchr(exe_path, L'\\');
    if (!filename)
        return false;
    filename[1] = L'\0';

    wchar_t crash_directory[MAX_PATH];
    if (join_path(crash_directory, MAX_PATH, exe_path, L"crashes\\") &&
        make_directory(crash_directory)) {
        wchar_t probe[MAX_PATH];
        if (join_path(probe, MAX_PATH, crash_directory, L"hua-crash-write.probe")) {
            HANDLE file = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
            if (file != INVALID_HANDLE_VALUE) {
                BOOL close_ok = CloseHandle(file);
                BOOL delete_ok = DeleteFileW(probe);
                if (close_ok && delete_ok)
                    return init_in_directory(crash_directory);
            }
        }
    }

    wchar_t local_directory[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL,
                         SHGFP_TYPE_CURRENT, local_directory) != S_OK)
        return false;
    wchar_t hua_directory[MAX_PATH];
    if (!join_path(hua_directory, MAX_PATH, local_directory, L"\\Hua"))
        return false;
    if (!make_directory(hua_directory))
        return false;
    wchar_t crash_dump_directory[MAX_PATH];
    if (!join_path(crash_dump_directory, MAX_PATH, hua_directory, L"\\CrashDumps"))
        return false;
    if (!make_directory(crash_dump_directory))
        return false;
    return init_in_directory(crash_dump_directory);
}

const char *crash_diag_previous_run(void)
{
    return g_previous_run[0] ? g_previous_run : NULL;
}

void crash_diag_mark_clean_shutdown(void)
{
    if (!g_initialized || !g_marker_path[0])
        return;
    g_clean = DeleteFileW(g_marker_path) != 0;
}

void crash_diag_shutdown(void)
{
    g_initialized = false;
}

#ifdef HUA_CRASH_DIAG_TESTING
bool crash_diag_init_in_directory_for_test(const wchar_t *directory)
{
    return init_in_directory(directory);
}
#endif
