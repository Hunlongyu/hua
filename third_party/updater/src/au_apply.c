/*
 * au_apply.c — 替换与重启，三种模式；单实例握手；重启后清理。
 *
 * 自替换的原子性依赖「重命名正在运行的 exe 是允许的，删除才被禁止」这一 Windows 行为。
 * 临时文件必须与 exe 同卷（au_download 已保证），否则 MoveFileExW 跨卷退化为 copy+delete。
 */
#include "au_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

/* 取本进程的命令行参数（去掉 argv[0]），供重启时透传。返回宽字符串，调用方 free。 */
static wchar_t* forward_args(void) {
    const wchar_t* full = GetCommandLineW();
    /* 跳过 argv[0]：处理带引号的可执行路径。 */
    const wchar_t* p = full;
    if (*p == L'"') { p++; while (*p && *p != L'"') p++; if (*p == L'"') p++; }
    else            { while (*p && *p != L' ' && *p != L'\t') p++; }
    while (*p == L' ' || *p == L'\t') p++;
    return _wcsdup(p);
}

/* 组装带 --au-wait-pid 的命令行并启动 exe。 */
static au_err_t launch_new(au_ctx_t* ctx, const wchar_t* exe_path) {
    wchar_t* args = forward_args();
    DWORD pid = GetCurrentProcessId();

    size_t need = wcslen(exe_path) + (args ? wcslen(args) : 0) + 64;
    wchar_t* cmd = (wchar_t*)malloc(need * sizeof(wchar_t));
    if (!cmd) { free(args); return AU_ERR_NOMEM; }
    /* "exe" --au-wait-pid=<pid> <原参数> */
    swprintf(cmd, need, L"\"%s\" --au-wait-pid=%lu %s", exe_path, pid, args ? args : L"");
    free(args);

    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    wchar_t* dir = au_dir_of(exe_path);
    BOOL ok = CreateProcessW(exe_path, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi);
    free(dir); free(cmd);
    if (!ok) { au_logf(ctx, AU_LOG_ERROR, "launch new exe failed: %lu", GetLastError()); return AU_ERR_LAUNCH; }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return AU_OK;
}

/* ---------- 模式 A：单文件替换 ---------- */
static au_err_t apply_exe(au_ctx_t* ctx, const wchar_t* pkg) {
    wchar_t* self = au_self_path();
    if (!self) return AU_ERR_STATE;
    wchar_t* dir = au_dir_of(self);
    if (!dir || !au_dir_writable(dir)) {
        au_logf(ctx, AU_LOG_ERROR, "install dir not writable");
        free(self); free(dir); return AU_ERR_NOT_WRITABLE;
    }

    wchar_t oldp[MAX_PATH];
    _snwprintf(oldp, MAX_PATH, L"%s.old", self); oldp[MAX_PATH-1]=L'\0';
    DeleteFileW(oldp);   /* 清理可能的上次残留 */

    /* 现役 exe → .old */
    if (!MoveFileExW(self, oldp, MOVEFILE_REPLACE_EXISTING)) {
        au_logf(ctx, AU_LOG_ERROR, "rename self→.old failed: %lu", GetLastError());
        free(self); free(dir); return AU_ERR_REPLACE;
    }
    /* 新版 → 原路径 */
    if (!MoveFileExW(pkg, self, MOVEFILE_REPLACE_EXISTING)) {
        au_logf(ctx, AU_LOG_ERROR, "rename new→self failed: %lu; rolling back", GetLastError());
        MoveFileExW(oldp, self, MOVEFILE_REPLACE_EXISTING);  /* 回滚 */
        free(self); free(dir); return AU_ERR_REPLACE;
    }

    au_err_t rc = launch_new(ctx, self);
    free(self); free(dir);
    if (rc != AU_OK) return rc;
    ExitProcess(0);
    return AU_OK;   /* 不会到达 */
}

/* ---------- 模式 B：ZIP 完整包 ---------- */
static au_err_t apply_zip(au_ctx_t* ctx, const wchar_t* pkg) {
    wchar_t* self = au_self_path();
    if (!self) return AU_ERR_STATE;
    wchar_t* dir = au_dir_of(self);
    if (!dir || !au_dir_writable(dir)) { free(self); free(dir); return AU_ERR_NOT_WRITABLE; }

    wchar_t tmp[MAX_PATH], extract[MAX_PATH];
    _snwprintf(tmp, MAX_PATH, L"%s\\_au_tmp", dir); tmp[MAX_PATH-1]=L'\0';
    _snwprintf(extract, MAX_PATH, L"%s\\extract", tmp); extract[MAX_PATH-1]=L'\0';
    au_rmtree(tmp);
    CreateDirectoryW(tmp, NULL);

    /* 先全部解压 + 校验，再动安装目录。 */
    au_err_t rc = au_extract_zip(ctx, pkg, extract);
    if (rc != AU_OK) { au_rmtree(tmp); free(self); free(dir); return rc; }

    /* 校验解压产物里有主 exe（basename 与自身一致），防止发错包。 */
    wchar_t* selfname = wcsrchr(self, L'\\');
    selfname = selfname ? selfname + 1 : self;
    wchar_t mainexe[MAX_PATH];
    _snwprintf(mainexe, MAX_PATH, L"%s\\%s", extract, selfname); mainexe[MAX_PATH-1]=L'\0';
    if (!au_file_exists(mainexe)) {
        au_logf(ctx, AU_LOG_ERROR, "extracted package missing main exe %ls", selfname);
        au_rmtree(tmp); free(self); free(dir); return AU_ERR_EXTRACT;
    }

    /* 现役 exe → .old（释放占用，供覆盖）。 */
    wchar_t oldp[MAX_PATH];
    _snwprintf(oldp, MAX_PATH, L"%s.old", self); oldp[MAX_PATH-1]=L'\0';
    DeleteFileW(oldp);
    if (!MoveFileExW(self, oldp, MOVEFILE_REPLACE_EXISTING)) {
        au_rmtree(tmp); free(self); free(dir); return AU_ERR_REPLACE;
    }

    /* 覆盖复制（zip_exclude 保护的已存在文件跳过）。 */
    rc = au_copy_tree(ctx, extract, dir);
    if (rc != AU_OK) {
        /* 尽力把自身 exe 恢复回来，至少保证程序还能启动。 */
        if (!au_file_exists(self)) MoveFileExW(oldp, self, MOVEFILE_REPLACE_EXISTING);
        au_rmtree(tmp); free(self); free(dir); return rc;
    }

    rc = launch_new(ctx, self);
    au_rmtree(tmp);
    free(self); free(dir);
    if (rc != AU_OK) return rc;
    ExitProcess(0);
    return AU_OK;
}

/* ---------- 模式 C：安装器 ---------- */
static au_err_t apply_installer(au_ctx_t* ctx, const wchar_t* pkg) {
    /* 组装安装器命令行：优先 installer_args，其次按 silent 推断 NSIS "/S"。 */
    const wchar_t* extra = L"";
    wchar_t* extra_owned = NULL;
    if (ctx->installer_args && ctx->installer_args[0]) {
        extra_owned = au_utf8_to_wide(ctx->installer_args);
        extra = extra_owned ? extra_owned : L"";
    } else if (ctx->installer_silent) {
        extra = L"/S";   /* NSIS 静默 */
    }

    size_t need = wcslen(pkg) + wcslen(extra) + 8;
    wchar_t* cmd = (wchar_t*)malloc(need * sizeof(wchar_t));
    if (!cmd) { free(extra_owned); return AU_ERR_NOMEM; }
    swprintf(cmd, need, L"\"%s\" %s", pkg, extra);

    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    /* 安装器通常需要 UAC 提权；用 ShellExecute 语义更稳，但 CreateProcess 在已提权
     * 进程（如 hua）下继承令牌即可。这里用 CreateProcess，失败再由调用方决定。 */
    BOOL ok = CreateProcessW(pkg, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmd); free(extra_owned);
    if (!ok) { au_logf(ctx, AU_LOG_ERROR, "launch installer failed: %lu", GetLastError()); return AU_ERR_LAUNCH; }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    /* 安装器自行完成替换与拉起新版；本进程退出以释放文件占用。 */
    ExitProcess(0);
    return AU_OK;
}

au_err_t au_apply(au_ctx_t* ctx, const char* pkg_path) {
    if (!ctx || !pkg_path) return AU_ERR_ARG;
    wchar_t* pkg = au_utf8_to_wide(pkg_path);
    if (!pkg) return AU_ERR_NOMEM;
    au_err_t rc;
    switch (ctx->mode) {
        case AU_MODE_EXE:       rc = apply_exe(ctx, pkg); break;
        case AU_MODE_ZIP:       rc = apply_zip(ctx, pkg); break;
        case AU_MODE_INSTALLER: rc = apply_installer(ctx, pkg); break;
        default:                rc = AU_ERR_ARG; break;
    }
    free(pkg);
    return rc;
}

/* ---------- 进程握手 ---------- */

/* 从一个 --au-wait-pid=<n> 参数取 PID，非该参数返回 0。 */
static DWORD parse_wait_pid_w(const wchar_t* arg) {
    const wchar_t* pfx = L"--au-wait-pid=";
    size_t plen = wcslen(pfx);
    if (wcsncmp(arg, pfx, plen) == 0) return (DWORD)_wtoi(arg + plen);
    return 0;
}

static void wait_pid(DWORD pid, int timeout_ms) {
    if (pid == 0) return;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return;   /* 已退出或无权限 */
    WaitForSingleObject(h, timeout_ms > 0 ? (DWORD)timeout_ms : 10000);
    CloseHandle(h);
}

void au_wait_for_predecessor_w(int argc, wchar_t** wargv, int timeout_ms) {
    for (int i = 1; i < argc; i++) {
        DWORD pid = parse_wait_pid_w(wargv[i]);
        if (pid) { wait_pid(pid, timeout_ms); return; }
    }
}

void au_wait_for_predecessor(int argc, char** argv, int timeout_ms) {
    for (int i = 1; i < argc; i++) {
        wchar_t* w = au_utf8_to_wide(argv[i]);
        if (!w) continue;
        DWORD pid = parse_wait_pid_w(w);
        free(w);
        if (pid) { wait_pid(pid, timeout_ms); return; }
    }
}

/* ---------- 重启后清理 ---------- */
void au_cleanup_after_restart(void) {
    wchar_t* self = au_self_path();
    if (!self) return;
    wchar_t oldp[MAX_PATH];
    _snwprintf(oldp, MAX_PATH, L"%s.old", self); oldp[MAX_PATH-1]=L'\0';
    if (au_file_exists(oldp)) {
        SetFileAttributesW(oldp, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(oldp);   /* 可能因旧进程尚未完全退出而暂失败；下次启动再试，无害 */
    }
    wchar_t* dir = au_dir_of(self);
    if (dir) {
        wchar_t tmp[MAX_PATH];
        _snwprintf(tmp, MAX_PATH, L"%s\\_au_tmp", dir); tmp[MAX_PATH-1]=L'\0';
        if (au_file_exists(tmp)) au_rmtree(tmp);
        free(dir);
    }
    free(self);
}
