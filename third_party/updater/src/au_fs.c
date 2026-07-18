/*
 * au_fs.c — 文件系统操作：自身路径、目录可写探测、tar.exe 解压、
 * 带排除表的目录树复制、递归删除。
 */
#include "au_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

wchar_t* au_self_path(void) {
    DWORD cap = 512;
    for (;;) {
        wchar_t* buf = (wchar_t*)malloc(cap * sizeof(wchar_t));
        if (!buf) return NULL;
        DWORD n = GetModuleFileNameW(NULL, buf, cap);
        if (n == 0) { free(buf); return NULL; }
        if (n < cap) return buf;           /* 完整写入 */
        free(buf); cap *= 2;               /* 截断，扩容重试 */
        if (cap > 1u << 16) return NULL;
    }
}

wchar_t* au_dir_of(const wchar_t* path) {
    if (!path) return NULL;
    wchar_t* copy = _wcsdup(path);
    if (!copy) return NULL;
    wchar_t* slash = wcsrchr(copy, L'\\');
    wchar_t* fslash = wcsrchr(copy, L'/');
    if (fslash && (!slash || fslash > slash)) slash = fslash;
    if (slash) *slash = L'\0';
    else { free(copy); return _wcsdup(L"."); }
    return copy;
}

int au_file_exists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES;
}

int au_dir_writable(const wchar_t* dir) {
    wchar_t probe[MAX_PATH];
    /* 用固定名 + PID 降低碰撞（不用随机数：环境禁用了随机源，PID 足够）。 */
    _snwprintf(probe, MAX_PATH, L"%s\\.au_wtest_%lu", dir, GetCurrentProcessId());
    probe[MAX_PATH-1] = L'\0';
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);   /* DELETE_ON_CLOSE 自动清理 */
    return 1;
}

/* 找系统 tar.exe 全路径（%SystemRoot%\System32\tar.exe），避免 PATH 劫持。 */
static int system_tar_path(wchar_t* out, size_t cap) {
    UINT n = GetSystemDirectoryW(out, (UINT)cap);
    if (n == 0 || n >= cap) return 0;
    if (wcscat_s(out, cap, L"\\tar.exe") != 0) return 0;
    return au_file_exists(out);
}

au_err_t au_extract_zip(au_ctx_t* ctx, const wchar_t* zip, const wchar_t* dest_dir) {
    wchar_t tar[MAX_PATH];
    if (!system_tar_path(tar, MAX_PATH)) {
        au_logf(ctx, AU_LOG_ERROR, "system tar.exe not found (need Win10 1803+)");
        return AU_ERR_EXTRACT;
    }
    CreateDirectoryW(dest_dir, NULL);

    /* 命令行：tar.exe -xf "zip" -C "dest"。手工加引号防空格。 */
    wchar_t* cmd = (wchar_t*)malloc((wcslen(tar) + wcslen(zip) + wcslen(dest_dir) + 32) * sizeof(wchar_t));
    if (!cmd) return AU_ERR_NOMEM;
    swprintf(cmd, wcslen(tar) + wcslen(zip) + wcslen(dest_dir) + 32,
             L"\"%s\" -xf \"%s\" -C \"%s\"", tar, zip, dest_dir);

    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd);
    if (!ok) { au_logf(ctx, AU_LOG_ERROR, "CreateProcess tar failed: %lu", GetLastError()); return AU_ERR_EXTRACT; }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (code != 0) { au_logf(ctx, AU_LOG_ERROR, "tar exit code %lu", code); return AU_ERR_EXTRACT; }
    return AU_OK;
}

/* basename 是否命中排除表（大小写不敏感）。 */
static int is_excluded(au_ctx_t* ctx, const wchar_t* name) {
    for (size_t i = 0; i < ctx->zip_exclude_count; i++)
        if (_wcsicmp(name, ctx->zip_exclude[i]) == 0) return 1;
    return 0;
}

au_err_t au_copy_tree(au_ctx_t* ctx, const wchar_t* src_dir, const wchar_t* dst_dir) {
    CreateDirectoryW(dst_dir, NULL);

    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*", src_dir);
    pattern[MAX_PATH-1] = L'\0';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return AU_OK;   /* 空目录 */

    au_err_t rc = AU_OK;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        wchar_t sp[MAX_PATH], dp[MAX_PATH];
        _snwprintf(sp, MAX_PATH, L"%s\\%s", src_dir, fd.cFileName); sp[MAX_PATH-1]=L'\0';
        _snwprintf(dp, MAX_PATH, L"%s\\%s", dst_dir, fd.cFileName); dp[MAX_PATH-1]=L'\0';

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            rc = au_copy_tree(ctx, sp, dp);
            if (rc != AU_OK) break;
        } else {
            /* 排除表：仅当目标已存在时跳过（保护用户配置）。 */
            if (is_excluded(ctx, fd.cFileName) && au_file_exists(dp)) {
                au_logf(ctx, AU_LOG_INFO, "skip existing (excluded): %ls", fd.cFileName);
                continue;
            }
            if (!CopyFileW(sp, dp, FALSE)) {
                au_logf(ctx, AU_LOG_ERROR, "copy failed: %ls (%lu)", fd.cFileName, GetLastError());
                rc = AU_ERR_REPLACE; break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return rc;
}

void au_rmtree(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*", dir); pattern[MAX_PATH-1]=L'\0';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t p[MAX_PATH];
            _snwprintf(p, MAX_PATH, L"%s\\%s", dir, fd.cFileName); p[MAX_PATH-1]=L'\0';
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) au_rmtree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}
