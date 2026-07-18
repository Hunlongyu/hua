/*
 * au_update.c — 上下文生命周期与配置深拷贝。检测/下载/应用的具体实现分散在
 * au_gh.c / au_download.c / au_apply.c，本文件只负责持有状态与释放。
 */
#include "au_internal.h"
#include <stdlib.h>
#include <string.h>

/* 内置默认镜像模板。{url} 为完整原始 URL 占位符。
 * 官方直连优先；其余为公共反代，可能随时间失效，故设计为可被 cfg.mirrors 完全覆盖。 */
static const wchar_t* DEFAULT_MIRRORS[] = {
    L"{url}",
    L"https://gh-proxy.com/{url}",
    L"https://ghfast.top/{url}",
};

const char* au_strerror(au_err_t e) {
    switch (e) {
        case AU_OK:              return "ok";
        case AU_ERR_ARG:         return "invalid argument";
        case AU_ERR_NOMEM:       return "out of memory";
        case AU_ERR_NET:         return "network error (all mirrors failed)";
        case AU_ERR_HTTP:        return "http error";
        case AU_ERR_RATELIMIT:   return "github api rate limited";
        case AU_ERR_PARSE:       return "response parse error";
        case AU_ERR_NO_ASSET:    return "no matching asset";
        case AU_ERR_IO:          return "file io error";
        case AU_ERR_HASH:        return "sha-256 mismatch";
        case AU_ERR_EXTRACT:     return "zip extract failed";
        case AU_ERR_NOT_WRITABLE:return "install dir not writable";
        case AU_ERR_REPLACE:     return "file replace failed";
        case AU_ERR_LAUNCH:      return "launch process failed";
        case AU_ERR_CANCELED:    return "canceled";
        case AU_ERR_STATE:       return "internal state error";
        default:                 return "unknown error";
    }
}

/* 小写化一份宽字符串（用于 zip_exclude 比较预处理）。此处保留原样，比较时用 _wcsicmp。 */

au_ctx_t* au_create(const au_config_t* cfg) {
    if (!cfg || !cfg->owner || !cfg->repo || !cfg->current_version ||
        !cfg->asset_pattern || cfg->mode == 0)
        return NULL;

    au_ctx_t* c = (au_ctx_t*)calloc(1, sizeof *c);
    if (!c) return NULL;

    c->owner         = au_strdup(cfg->owner);
    c->repo          = au_strdup(cfg->repo);
    c->asset_pattern = au_strdup(cfg->asset_pattern);
    c->user_agent    = au_strdup(cfg->user_agent ? cfg->user_agent : "WinAutoUpdate/1.0");
    c->token         = cfg->token ? au_strdup(cfg->token) : NULL;
    c->installer_args= cfg->installer_args ? au_strdup(cfg->installer_args) : NULL;

    if (au_version_parse(cfg->current_version, &c->current) != AU_OK) { au_destroy(c); return NULL; }

    c->mode             = cfg->mode;
    c->channel          = cfg->channel;
    c->timeout_ms       = cfg->timeout_ms > 0 ? cfg->timeout_ms : 30000;
    c->retry_max        = cfg->retry_max > 0 ? cfg->retry_max : 2;
    c->installer_silent = cfg->installer_silent;
    c->on_progress      = cfg->on_progress;
    c->on_log           = cfg->on_log;
    c->userdata         = cfg->userdata;
    c->cancel_flag      = 0;

    /* 镜像：用户提供则用之，否则用内置默认。全部转宽字符深拷贝。 */
    const void* srcm; size_t nm;
    if (cfg->mirrors && cfg->mirror_count > 0) {
        nm = cfg->mirror_count;
        c->mirrors = (wchar_t**)calloc(nm, sizeof(wchar_t*));
        if (!c->mirrors) { au_destroy(c); return NULL; }
        for (size_t i = 0; i < nm; i++) c->mirrors[i] = au_utf8_to_wide(cfg->mirrors[i]);
        (void)srcm;
    } else {
        nm = sizeof DEFAULT_MIRRORS / sizeof DEFAULT_MIRRORS[0];
        c->mirrors = (wchar_t**)calloc(nm, sizeof(wchar_t*));
        if (!c->mirrors) { au_destroy(c); return NULL; }
        for (size_t i = 0; i < nm; i++) c->mirrors[i] = _wcsdup(DEFAULT_MIRRORS[i]);
    }
    c->mirror_count = nm;

    /* zip_exclude：转宽字符深拷贝。 */
    if (cfg->zip_exclude && cfg->zip_exclude_count > 0) {
        c->zip_exclude = (wchar_t**)calloc(cfg->zip_exclude_count, sizeof(wchar_t*));
        if (!c->zip_exclude) { au_destroy(c); return NULL; }
        for (size_t i = 0; i < cfg->zip_exclude_count; i++)
            c->zip_exclude[i] = au_utf8_to_wide(cfg->zip_exclude[i]);
        c->zip_exclude_count = cfg->zip_exclude_count;
    }

    if (!c->owner || !c->repo || !c->asset_pattern || !c->user_agent) { au_destroy(c); return NULL; }
    return c;
}

void au_destroy(au_ctx_t* c) {
    if (!c) return;
    free(c->owner); free(c->repo); free(c->asset_pattern);
    free(c->user_agent); free(c->token); free(c->installer_args);
    if (c->mirrors) { for (size_t i = 0; i < c->mirror_count; i++) free(c->mirrors[i]); free(c->mirrors); }
    if (c->zip_exclude) { for (size_t i = 0; i < c->zip_exclude_count; i++) free(c->zip_exclude[i]); free(c->zip_exclude); }
    free(c);
}

void au_cancel(au_ctx_t* c) {
    if (c) InterlockedExchange(&c->cancel_flag, 1);
}
