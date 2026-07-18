/*
 * au_str.c — 编码转换、glob 匹配、日志格式化。纯逻辑（除 Win32 编码 API），
 * 无网络、无文件句柄，是全库最好测的部分之一。
 */
#include "au_internal.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char* au_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

wchar_t* au_utf8_to_wide(const char* s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    return w;
}

char* au_wide_to_utf8(const wchar_t* w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char* s = (char*)malloc((size_t)n);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) { free(s); return NULL; }
    return s;
}

/* 大小写不敏感的 glob，支持 * 与 ?。经典回溯法，避免递归爆栈。
 * 对 ASCII 命名的资产名足够；中文等多字节按字节匹配（* 仍能跨越）。 */
int au_glob_match(const char* pat, const char* str) {
    if (!pat || !str) return 0;
    const char *p = pat, *s = str;
    const char *star_p = NULL, *star_s = NULL;
    while (*s) {
        if (*p == '*') {
            star_p = ++p;          /* 记住 * 之后的位置 */
            star_s = s;            /* 记住此刻的文本位置 */
        } else if (*p == '?' ||
                   tolower((unsigned char)*p) == tolower((unsigned char)*s)) {
            ++p; ++s;
        } else if (star_p) {
            p = star_p;            /* 回溯：让 * 多吞一个字符 */
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*') ++p;         /* 尾部剩余的 * 匹配空串 */
    return *p == '\0';
}

void au_logf(au_ctx_t* ctx, au_log_level_t lv, const char* fmt, ...) {
    if (!ctx || !ctx->on_log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = '\0';
    ctx->on_log(lv, buf, ctx->userdata);
}
