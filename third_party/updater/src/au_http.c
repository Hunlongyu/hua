/*
 * au_http.c — WinHTTP 封装。HTTPS GET 到内存或文件，进度回调，取消，
 * 请求头，限流头解析。TLS 与证书链校验由 WinHTTP 内部完成。
 */
#include "au_internal.h"
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

/* 把 UTF-8 headers 拼进请求。GitHub 强制要求 User-Agent。 */
static wchar_t* build_headers(au_ctx_t* ctx, int is_api) {
    /* 组装成一整块，用 \r\n 分隔。 */
    char tmp[2048];
    int n = 0;
    n += snprintf(tmp + n, sizeof tmp - n, "User-Agent: %s\r\n",
                  ctx->user_agent ? ctx->user_agent : "WinAutoUpdate/1.0");
    if (is_api) {
        n += snprintf(tmp + n, sizeof tmp - n, "Accept: application/vnd.github+json\r\n");
        n += snprintf(tmp + n, sizeof tmp - n, "X-GitHub-Api-Version: 2022-11-28\r\n");
    }
    if (ctx->token && ctx->token[0])
        n += snprintf(tmp + n, sizeof tmp - n, "Authorization: Bearer %s\r\n", ctx->token);
    (void)n;
    return au_utf8_to_wide(tmp);
}

/* 读取限流头 X-RateLimit-Remaining。缺失置 -1。 */
static int read_ratelimit(HINTERNET hReq) {
    wchar_t buf[32]; DWORD len = sizeof buf;
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CUSTOM,
                            L"X-RateLimit-Remaining", buf, &len, WINHTTP_NO_HEADER_INDEX)) {
        return (int)_wtoi(buf);
    }
    return -1;
}

/* 核心请求。
 *  - out_mem 非 NULL：响应体写入新分配的内存缓冲。
 *  - 否则 hSink 为文件句柄，流式落盘。
 *  - out_location 非 NULL：**不跟随重定向**，遇 3xx 时把 Location 头（UTF-8）写入
 *    *out_location 并即刻返回 AU_OK（不读响应体）。用于读取 github.com 网页端点
 *    releases/latest 的 302 目标以取回最新 tag（限流兜底路径）。 */
static au_err_t do_request(au_ctx_t* ctx, const wchar_t* url, int is_api,
                           char** out_mem, size_t* out_len,
                           HANDLE hSink, char** out_location, au_http_meta* meta) {
    au_err_t rc = AU_ERR_NET;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    wchar_t host[256] = {0}, path[4096] = {0};
    wchar_t* headers = NULL;
    char* mem = NULL; size_t cap = 0, len = 0;

    if (meta) { meta->status = 0; meta->ratelimit_remaining = -1; }

    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = sizeof host / sizeof host[0];
    uc.lpszUrlPath = path; uc.dwUrlPathLength = sizeof path / sizeof path[0];
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return AU_ERR_ARG;

    int https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    hSession = WinHttpOpen(L"WinAutoUpdate/1.0",
                           WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto done;

    DWORD tmo = (DWORD)(ctx->timeout_ms > 0 ? ctx->timeout_ms : 30000);
    WinHttpSetTimeouts(hSession, (int)tmo, (int)tmo, (int)tmo, (int)tmo);
    /* 强制 TLS1.2/1.3。 */
    DWORD secure = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS1_3 */;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secure, sizeof secure);

    hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) goto done;

    hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) goto done;

    /* 常规请求跟随重定向（跨主机自动丢弃 Authorization）；读取 Location 模式下则
     * 禁止跟随，自己拿 302 的目标。 */
    DWORD redirect = out_location ? WINHTTP_OPTION_REDIRECT_POLICY_NEVER
                                  : WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);

    headers = build_headers(ctx, is_api);
    if (!WinHttpSendRequest(hRequest, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                            headers ? (DWORD)-1L : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto done;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto done;

    /* 状态码 */
    DWORD status = 0, slen = sizeof status;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
    int ratelimit = read_ratelimit(hRequest);
    if (meta) { meta->status = (int)status; meta->ratelimit_remaining = ratelimit; }

    if (status == 403 && ratelimit == 0) { rc = AU_ERR_RATELIMIT; goto done; }

    /* 读取 Location 模式：只要 302 的目标，不读体。 */
    if (out_location) {
        if (status >= 300 && status < 400) {
            wchar_t loc[2048]; DWORD ll = sizeof loc;
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, loc, &ll,
                                    WINHTTP_NO_HEADER_INDEX)) {
                *out_location = au_wide_to_utf8(loc);
                rc = *out_location ? AU_OK : AU_ERR_NOMEM;
            } else {
                rc = AU_ERR_HTTP;   /* 3xx 却无 Location 头 */
            }
        } else {
            rc = AU_ERR_HTTP;       /* 期望重定向却非 3xx（如仓库无 release 时 404） */
        }
        goto done;
    }

    if (status < 200 || status >= 300)  { rc = AU_ERR_HTTP; goto done; }

    /* 总长度（下载进度用；可能缺失）。 */
    uint64_t total = 0;
    {
        wchar_t clen[32]; DWORD cl = sizeof clen;
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, clen, &cl, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(clen, NULL, 10);
    }

    /* 读循环。done 统计已落地字节，供文件模式的进度回调。 */
    uint64_t done_bytes = 0;
    for (;;) {
        if (au_is_canceled(ctx)) { rc = AU_ERR_CANCELED; goto done; }

        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) { rc = AU_ERR_NET; goto done; }
        if (avail == 0) break;

        char stackbuf[16384];
        DWORD want = avail > sizeof stackbuf ? sizeof stackbuf : avail;
        DWORD got = 0;
        if (!WinHttpReadData(hRequest, stackbuf, want, &got)) { rc = AU_ERR_NET; goto done; }
        if (got == 0) break;

        if (out_mem) {
            if (len + got + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 65536;
                while (ncap < len + got + 1) ncap *= 2;
                char* np = (char*)realloc(mem, ncap);
                if (!np) { rc = AU_ERR_NOMEM; goto done; }
                mem = np; cap = ncap;
            }
            memcpy(mem + len, stackbuf, got);
            len += got;
        } else {
            DWORD written = 0;
            if (!WriteFile(hSink, stackbuf, got, &written, NULL) || written != got) {
                rc = AU_ERR_IO; goto done;
            }
            done_bytes += got;
            if (ctx->on_progress) {
                /* total 可能为 0，原样传出，不编造百分比。 */
                if (ctx->on_progress(total, done_bytes, ctx->userdata) != 0) {
                    rc = AU_ERR_CANCELED; goto done;
                }
            }
        }
    }

    if (out_mem) {
        if (!mem) { mem = (char*)malloc(1); cap = 1; }
        mem[len] = '\0';
        *out_mem = mem; *out_len = len; mem = NULL;
    }
    rc = AU_OK;

done:
    if (mem) free(mem);
    if (headers) free(headers);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return rc;
}

au_err_t au_http_get_mem(au_ctx_t* ctx, const wchar_t* url,
                         char** out, size_t* len, au_http_meta* meta) {
    if (!ctx || !url || !out || !len) return AU_ERR_ARG;
    return do_request(ctx, url, 1, out, len, NULL, NULL, meta);
}

/* 网页端点取内存（非 API：不发 Accept/X-GitHub-Api-Version）。用于抓 checksums.txt。 */
au_err_t au_http_get_mem_web(au_ctx_t* ctx, const wchar_t* url,
                             char** out, size_t* len, au_http_meta* meta) {
    if (!ctx || !url || !out || !len) return AU_ERR_ARG;
    return do_request(ctx, url, 0, out, len, NULL, NULL, meta);
}

au_err_t au_http_get_file(au_ctx_t* ctx, const wchar_t* url, HANDLE hFile,
                          au_http_meta* meta) {
    if (!ctx || !url || hFile == INVALID_HANDLE_VALUE) return AU_ERR_ARG;
    return do_request(ctx, url, 0, NULL, NULL, hFile, NULL, meta);
}

/* GET url 但不跟随重定向，取回 3xx 的 Location 头（UTF-8，新分配，调用方 free）。 */
au_err_t au_http_get_location(au_ctx_t* ctx, const wchar_t* url,
                              char** out_location, au_http_meta* meta) {
    if (!ctx || !url || !out_location) return AU_ERR_ARG;
    *out_location = NULL;
    return do_request(ctx, url, 0, NULL, NULL, NULL, out_location, meta);
}

/* {url} 模板替换。 */
static wchar_t* apply_template(const wchar_t* tmpl, const wchar_t* raw) {
    const wchar_t* marker = wcsstr(tmpl, L"{url}");
    if (!marker) {
        /* 无占位符：整串即最终 URL（第一项 "{url}" 之外的少见配置）。 */
        return _wcsdup(tmpl);
    }
    size_t pre = (size_t)(marker - tmpl);
    size_t rawlen = wcslen(raw);
    size_t taillen = wcslen(marker + 5);
    wchar_t* out = (wchar_t*)malloc((pre + rawlen + taillen + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    wmemcpy(out, tmpl, pre);
    wmemcpy(out + pre, raw, rawlen);
    wmemcpy(out + pre + rawlen, marker + 5, taillen);
    out[pre + rawlen + taillen] = L'\0';
    return out;
}

wchar_t** au_build_mirror_urls(au_ctx_t* ctx, const char* raw_url, size_t* count) {
    wchar_t* raw = au_utf8_to_wide(raw_url);
    if (!raw) { *count = 0; return NULL; }

    size_t n = ctx->mirror_count;
    wchar_t** urls = (wchar_t**)calloc(n, sizeof(wchar_t*));
    if (!urls) { free(raw); *count = 0; return NULL; }
    for (size_t i = 0; i < n; i++)
        urls[i] = apply_template(ctx->mirrors[i], raw);
    free(raw);
    *count = n;
    return urls;
}
