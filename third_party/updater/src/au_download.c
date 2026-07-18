/*
 * au_download.c — 资产下载编排：镜像轮换、落到 exe 同目录（保证同卷）、SHA-256 校验。
 */
#include "au_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 从 asset name 取一个安全的临时落地文件名：<selfdir>\<name>.autmp */
static wchar_t* temp_target_path(au_ctx_t* ctx, const au_asset_t* a) {
    (void)ctx;
    wchar_t* self = au_self_path();
    if (!self) return NULL;
    wchar_t* dir = au_dir_of(self);
    free(self);
    if (!dir) return NULL;

    wchar_t* wname = au_utf8_to_wide(a->name ? a->name : "download.bin");
    if (!wname) { free(dir); return NULL; }

    size_t need = wcslen(dir) + wcslen(wname) + 16;
    wchar_t* path = (wchar_t*)malloc(need * sizeof(wchar_t));
    if (path) swprintf(path, need, L"%s\\%s.autmp", dir, wname);
    free(dir); free(wname);
    return path;
}

au_err_t au_download(au_ctx_t* ctx, const au_asset_t* a, char* out_path, size_t cap) {
    if (!ctx || !a || !a->url || !out_path) return AU_ERR_ARG;

    wchar_t* target = temp_target_path(ctx, a);
    if (!target) return AU_ERR_NOMEM;

    size_t nmir = 0;
    wchar_t** urls = au_build_mirror_urls(ctx, a->url, &nmir);
    if (!urls || nmir == 0) { free(urls); free(target); return AU_ERR_NOMEM; }

    au_err_t rc = AU_ERR_NET;
    for (size_t i = 0; i < nmir && rc != AU_OK && rc != AU_ERR_CANCELED; i++) {
        if (!urls[i]) continue;
        for (int attempt = 0; attempt <= ctx->retry_max; attempt++) {
            if (au_is_canceled(ctx)) { rc = AU_ERR_CANCELED; break; }

            HANDLE hFile = CreateFileW(target, GENERIC_WRITE, 0, NULL,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile == INVALID_HANDLE_VALUE) { rc = AU_ERR_IO; break; }

            au_http_meta meta;
            rc = au_http_get_file(ctx, urls[i], hFile, &meta);
            CloseHandle(hFile);

            if (rc == AU_OK) break;
            if (rc == AU_ERR_CANCELED) break;
            DeleteFileW(target);   /* 失败即清残片，下一轮重下 */
            au_logf(ctx, AU_LOG_WARN, "download attempt failed (mirror %zu, try %d): %s",
                    i, attempt, au_strerror(rc));
        }
    }

    if (rc != AU_OK) {
        DeleteFileW(target);
        free(target);
        for (size_t i = 0; i < nmir; i++) free(urls[i]);
        free(urls);
        return rc;
    }

    /* SHA-256 校验。 */
    if (a->sha256 && a->sha256[0]) {
        char got[65];
        au_err_t hr = au_sha256_file(target, got);
        if (hr != AU_OK) { rc = hr; }
        else if (_stricmp(got, a->sha256) != 0) {
            au_logf(ctx, AU_LOG_ERROR, "SHA-256 mismatch: got %s want %s", got, a->sha256);
            rc = AU_ERR_HASH;
        }
        if (rc != AU_OK) {
            DeleteFileW(target);
            free(target);
            for (size_t i = 0; i < nmir; i++) free(urls[i]);
            free(urls);
            return rc;
        }
    } else {
        au_logf(ctx, AU_LOG_WARN, "asset has no digest; skipping SHA-256 verification");
    }

    /* 回填 UTF-8 路径。 */
    char* u8 = au_wide_to_utf8(target);
    if (!u8 || strlen(u8) + 1 > cap) {
        free(u8); free(target);
        for (size_t i = 0; i < nmir; i++) free(urls[i]);
        free(urls);
        return AU_ERR_ARG;   /* 缓冲不够 */
    }
    memcpy(out_path, u8, strlen(u8) + 1);
    free(u8);

    free(target);
    for (size_t i = 0; i < nmir; i++) free(urls[i]);
    free(urls);
    return AU_OK;
}
