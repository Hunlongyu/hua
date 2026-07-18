/*
 * au_web.c — API 限流兜底：仅用 github.com 网页端点（不碰 REST API，不受 60/小时限额）。
 *
 * 触发时机见 au_gh.c：REST API 返回 AU_ERR_RATELIMIT 时才走这里。
 * 路径：
 *   1) GET github.com/<o>/<r>/releases/latest —— 不跟随重定向，读 302 的 Location，
 *      其形如 ".../releases/tag/<TAG>"，截出 <TAG> 即最新【正式】版（网页端点天然排除
 *      预发布，与 REST 的 releases/latest 语义一致）。
 *   2) 按约定拼资产直链 github.com/<o>/<r>/releases/download/<TAG>/<asset>。
 *   3) 抓 checksums.txt 填 asset->sha256——下载校验沿用 au_download 既有逻辑，不另立路径。
 *
 * 局限（均已记日志并如实降级/失败）：
 *   - 仅 stable 通道：网页端点只能给"最新正式版"，给不了"最新预发布"。
 *   - asset_pattern 必须是确定文件名（无 * / ?）：没有 API 就无法列资产、只能按名直拼。
 *   - 拿不到 API 的 digest，改用 checksums.txt；缺该文件则跳过校验（与老 release 同策略）。
 */
#include "au_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------- 纯函数（可离线单测）---------------- */

int au_web_tag_from_location(const char* location, char* out, size_t cap) {
    if (!location || !out || cap == 0) return 0;
    const char* key = "/releases/tag/";
    const char* p = strstr(location, key);
    if (!p) return 0;
    p += strlen(key);
    size_t n = 0;
    while (p[n] && p[n] != '/' && p[n] != '?' && p[n] != '#')
        n++;
    if (n == 0 || n + 1 > cap) return 0;   /* 空 tag 或放不下 */
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* 取路径的 basename（最后一个 '/' 或 '\' 之后）。 */
static const char* basename_of(const char* s) {
    const char* b = s;
    for (const char* p = s; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* tok 是否为恰好 64 位十六进制。 */
static int is_sha256_hex(const char* tok, size_t len) {
    if (len != 64) return 0;
    for (size_t i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)tok[i])) return 0;
    return 1;
}

int au_web_checksum_for(const char* checksums, const char* filename, char out_hex[65]) {
    if (!checksums || !filename || !out_hex) return 0;
    const char* want = basename_of(filename);

    const char* line = checksums;
    while (*line) {
        const char* eol = line;
        while (*eol && *eol != '\n') eol++;   /* 行 = [line, eol) */

        /* 跳过行首空白，取第一个 token 作为 hex。 */
        const char* p = line;
        while (p < eol && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        const char* hex = p;
        while (p < eol && *p != ' ' && *p != '\t' && *p != '\r') p++;
        size_t hexlen = (size_t)(p - hex);

        /* token 之间的空白；文件名可能带二进制标记 '*'。 */
        while (p < eol && (*p == ' ' || *p == '\t')) p++;
        if (p < eol && *p == '*') p++;
        const char* name = p;
        const char* nend = eol;
        while (nend > name && (nend[-1] == '\r' || nend[-1] == ' ' || nend[-1] == '\t'))
            nend--;

        if (hexlen == 64 && nend > name && is_sha256_hex(hex, hexlen)) {
            /* name 段（[name, nend)）的 basename：最后一个 '/' 或 '\' 之后。 */
            const char* nb = name;
            for (const char* q = name; q < nend; q++)
                if (*q == '/' || *q == '\\') nb = q + 1;
            size_t nblen = (size_t)(nend - nb);
            if (nblen == strlen(want) && _strnicmp(nb, want, nblen) == 0) {
                for (size_t i = 0; i < 64; i++)
                    out_hex[i] = (char)tolower((unsigned char)hex[i]);
                out_hex[64] = '\0';
                return 1;
            }
        }
        line = (*eol == '\n') ? eol + 1 : eol;
    }
    return 0;
}

/* ---------------- 兜底编排（联网）---------------- */

au_err_t au_fetch_web_fallback(au_ctx_t* ctx, au_release_t** out) {
    if (!ctx || !out) return AU_ERR_ARG;
    *out = NULL;

    if (ctx->channel != AU_CHANNEL_STABLE) {
        au_logf(ctx, AU_LOG_WARN, "web fallback: beta channel unsupported (no web endpoint for latest prerelease)");
        return AU_ERR_RATELIMIT;   /* 保留原始信号，调用方按限流处理 */
    }
    if (!ctx->asset_pattern || strpbrk(ctx->asset_pattern, "*?")) {
        au_logf(ctx, AU_LOG_WARN, "web fallback: asset_pattern must be a concrete name (no * or ?)");
        return AU_ERR_NO_ASSET;
    }

    /* 1) releases/latest → tag（不跟随重定向）。 */
    char url[512];
    snprintf(url, sizeof url, "https://github.com/%s/%s/releases/latest", ctx->owner, ctx->repo);
    wchar_t* wurl = au_utf8_to_wide(url);
    if (!wurl) return AU_ERR_NOMEM;

    char* location = NULL;
    au_http_meta meta;
    au_err_t rc = au_http_get_location(ctx, wurl, &location, &meta);
    free(wurl);
    if (rc != AU_OK) {
        au_logf(ctx, AU_LOG_WARN, "web fallback: releases/latest failed: %s", au_strerror(rc));
        return rc;
    }

    char tag[128];
    int ok = au_web_tag_from_location(location, tag, sizeof tag);
    free(location);
    if (!ok) {
        au_logf(ctx, AU_LOG_ERROR, "web fallback: cannot parse tag from redirect");
        return AU_ERR_PARSE;
    }

    au_version_t ver;
    if (au_version_parse(tag, &ver) != AU_OK) {
        au_logf(ctx, AU_LOG_ERROR, "web fallback: unparseable tag '%s'", tag);
        return AU_ERR_PARSE;
    }

    /* 2) 组装 release（单资产，按约定直拼下载链）。 */
    au_release_t* rel = (au_release_t*)calloc(1, sizeof *rel);
    if (!rel) return AU_ERR_NOMEM;
    rel->ver = ver;
    rel->is_prerelease = 0;
    rel->assets = (au_asset_t*)calloc(1, sizeof(au_asset_t));
    if (!rel->assets) { au_release_free(rel); return AU_ERR_NOMEM; }
    rel->asset_count = 1;
    rel->assets[0].name = au_strdup(ctx->asset_pattern);

    char asset_url[768];
    snprintf(asset_url, sizeof asset_url,
             "https://github.com/%s/%s/releases/download/%s/%s",
             ctx->owner, ctx->repo, tag, ctx->asset_pattern);
    rel->assets[0].url = au_strdup(asset_url);
    if (!rel->assets[0].name || !rel->assets[0].url) { au_release_free(rel); return AU_ERR_NOMEM; }

    /* 3) checksums.txt → sha256（尽力而为；缺失则 au_download 会告警跳过校验）。 */
    char cks_url[768];
    snprintf(cks_url, sizeof cks_url,
             "https://github.com/%s/%s/releases/download/%s/checksums.txt",
             ctx->owner, ctx->repo, tag);
    wchar_t* wcks = au_utf8_to_wide(cks_url);
    if (wcks) {
        char* body = NULL; size_t blen = 0;
        if (au_http_get_mem_web(ctx, wcks, &body, &blen, &meta) == AU_OK && body) {
            char hex[65];
            if (au_web_checksum_for(body, ctx->asset_pattern, hex))
                rel->assets[0].sha256 = au_strdup(hex);
            else
                au_logf(ctx, AU_LOG_WARN, "web fallback: no checksum entry for %s", ctx->asset_pattern);
            free(body);
        } else {
            au_logf(ctx, AU_LOG_WARN, "web fallback: checksums.txt unavailable; download will skip verify");
        }
        free(wcks);
    }

    au_logf(ctx, AU_LOG_INFO, "web fallback: latest is %s", tag);
    *out = rel;
    return AU_OK;
}
