/* test_crash_diag.c —— 运行状态标记生命周期测试。 */
#include "crash_diag.h"
#include "utest.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static bool make_test_dir(wchar_t *dir, size_t cap, const wchar_t *case_name)
{
    wchar_t temp[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, temp);
    if (len == 0 || len >= MAX_PATH)
        return false;
    _snwprintf(dir, cap, L"%s\\hua-crash-test-%lu-%s", temp,
               GetCurrentProcessId(), case_name);
    dir[cap - 1] = L'\0';
    return CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void join_path(wchar_t *path, size_t cap, const wchar_t *directory,
                      const wchar_t *name)
{
    _snwprintf(path, cap, L"%s\\%s", directory, name);
    path[cap - 1] = L'\0';
}

static bool file_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool write_utf8(const wchar_t *path, const char *text)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    size_t length = strlen(text);
    bool written_ok = WriteFile(file, text, (DWORD)length, &written, NULL) &&
                      written == (DWORD)length;
    return CloseHandle(file) && written_ok;
}

static bool read_utf8(const wchar_t *path, char *text, size_t cap)
{
    if (cap < 2)
        return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD read = 0;
    BOOL read_ok = ReadFile(file, text, (DWORD)(cap - 1), &read, NULL);
    BOOL close_ok = CloseHandle(file);
    if (!read_ok || !close_ok)
        return false;
    text[read] = '\0';
    return true;
}

static bool remove_test_dir(const wchar_t *directory)
{
    wchar_t marker[MAX_PATH], temp[MAX_PATH];
    join_path(marker, MAX_PATH, directory, L"hua-running.state");
    join_path(temp, MAX_PATH, directory, L"hua-running.state.tmp");
    DeleteFileW(marker);
    DeleteFileW(temp);
    return RemoveDirectoryW(directory) != 0;
}

UTEST(crash_diag, clean_shutdown_removes_marker)
{
    wchar_t dir[MAX_PATH], marker[MAX_PATH];
    ASSERT_TRUE(make_test_dir(dir, MAX_PATH, L"clean"));
    join_path(marker, MAX_PATH, dir, L"hua-running.state");

    ASSERT_TRUE(crash_diag_init_in_directory_for_test(dir));
    ASSERT_TRUE(file_exists(marker));
    ASSERT_TRUE(crash_diag_previous_run() == NULL);
    crash_diag_mark_clean_shutdown();
    crash_diag_shutdown();
    ASSERT_FALSE(file_exists(marker));
    ASSERT_TRUE(remove_test_dir(dir));
}

UTEST(crash_diag, leftover_marker_is_reported_without_calling_it_a_crash)
{
    wchar_t dir[MAX_PATH], marker[MAX_PATH];
    ASSERT_TRUE(make_test_dir(dir, MAX_PATH, L"unclean"));
    join_path(marker, MAX_PATH, dir, L"hua-running.state");
    ASSERT_TRUE(write_utf8(marker, "format=1\r\nversion=1.0.14\r\npid=1234\r\nstarted=2026-07-21T08:30:00.000+08:00\r\n"));

    ASSERT_TRUE(crash_diag_init_in_directory_for_test(dir));
    ASSERT_TRUE(strstr(crash_diag_previous_run(), "1.0.14") != NULL);
    ASSERT_TRUE(strstr(crash_diag_previous_run(), "1234") != NULL);
    crash_diag_mark_clean_shutdown();
    crash_diag_shutdown();
    ASSERT_TRUE(remove_test_dir(dir));
}

UTEST(crash_diag, marker_control_characters_do_not_reach_previous_run)
{
    wchar_t dir[MAX_PATH], marker[MAX_PATH];
    ASSERT_TRUE(make_test_dir(dir, MAX_PATH, L"control"));
    join_path(marker, MAX_PATH, dir, L"hua-running.state");
    ASSERT_TRUE(write_utf8(marker,
                           "version=1.0.14\tforged\r\npid=1234\r\n"
                           "started=2026-07-21T08:30:00.000+08:00\r\n"));

    ASSERT_TRUE(crash_diag_init_in_directory_for_test(dir));
    ASSERT_TRUE(crash_diag_previous_run() == NULL);
    crash_diag_mark_clean_shutdown();
    crash_diag_shutdown();
    ASSERT_TRUE(remove_test_dir(dir));
}

UTEST(crash_diag, unreadable_old_marker_aborts_without_replacing_it)
{
    static const char old_marker[] =
        "format=1\r\nversion=1.0.14\r\npid=1234\r\n"
        "started=2026-07-21T08:30:00.000+08:00\r\n";
    wchar_t dir[MAX_PATH], marker[MAX_PATH];
    char actual_marker[sizeof(old_marker)];
    ASSERT_TRUE(make_test_dir(dir, MAX_PATH, L"sharing"));
    join_path(marker, MAX_PATH, dir, L"hua-running.state");
    ASSERT_TRUE(write_utf8(marker, old_marker));

    /* Deny marker reads but permit deletion, so an implementation that mistakes
     * the sharing violation for absence can still replace this marker. */
    HANDLE lock = CreateFileW(marker, GENERIC_READ, FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    ASSERT_FALSE(crash_diag_init_in_directory_for_test(dir));
    ASSERT_TRUE(CloseHandle(lock));
    ASSERT_TRUE(read_utf8(marker, actual_marker, sizeof(actual_marker)));
    ASSERT_STREQ(old_marker, actual_marker);
    ASSERT_TRUE(remove_test_dir(dir));
}

UTEST_MAIN();
