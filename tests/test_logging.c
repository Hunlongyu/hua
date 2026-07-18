/* platform 日志策略集成测试：在本测试 exe 独立目录中验证轮转、保留期和关闭。 */
#include "platform.h"
#include "utest.h"

#include <string.h>

static void join_path(wchar_t *out, size_t cap, const wchar_t *dir, const wchar_t *name)
{
    _snwprintf(out, cap, L"%s%s", dir, name);
    out[cap - 1] = L'\0';
}

static void remove_test_logs(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    join_path(pattern, MAX_PATH, dir, L"hua*.log");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            join_path(path, MAX_PATH, dir, fd.cFileName);
            DeleteFileW(path);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static unsigned long long file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d))
        return 0;
    ULARGE_INTEGER n;
    n.LowPart = d.nFileSizeLow;
    n.HighPart = d.nFileSizeHigh;
    return n.QuadPart;
}

UTEST(logging, rotate_cleanup_and_off)
{
    wchar_t dir[MAX_PATH], current[MAX_PATH], stale[MAX_PATH], pattern[MAX_PATH];
    ASSERT_TRUE(hua_exe_dir(dir, MAX_PATH));
    remove_test_logs(dir);
    join_path(current, MAX_PATH, dir, L"hua.log");
    join_path(stale, MAX_PATH, dir, L"hua-20000101-000000-000.log");

    HANDLE old = CreateFileW(stale, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    ASSERT_TRUE(old != INVALID_HANDLE_VALUE);
    SYSTEMTIME old_st = {0};
    old_st.wYear = 2000; old_st.wMonth = 1; old_st.wDay = 1;
    FILETIME old_ft;
    ASSERT_TRUE(SystemTimeToFileTime(&old_st, &old_ft));
    ASSERT_TRUE(SetFileTime(old, &old_ft, &old_ft, &old_ft));
    CloseHandle(old);

    hua_log_configure(HUA_LOG_INFO, 1, 1);
    hua_log_init();
    ASSERT_FALSE(hua_file_exists(stale));

    char payload[901];
    memset(payload, 'x', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    for (int i = 0; i < 1400; i++)
        HUA_LOG_I("%04d %s", i, payload);
    hua_log_close();

    int archives = 0;
    join_path(pattern, MAX_PATH, dir, L"hua-*.log");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                archives++;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    ASSERT_TRUE(archives >= 1);
    ASSERT_TRUE(file_size(current) > 3);
    ASSERT_TRUE(file_size(current) <= 1024ULL * 1024ULL);

    unsigned long long before = file_size(current);
    hua_log_configure(HUA_LOG_OFF, 1, 1);
    hua_log_init();
    HUA_LOG_E("这一行不应写入");
    hua_log_close();
    ASSERT_EQ((long long)file_size(current), (long long)before);

    remove_test_logs(dir);
}

UTEST_MAIN();
