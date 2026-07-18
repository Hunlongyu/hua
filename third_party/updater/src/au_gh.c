/*
 * au_gh.c — GitHub API 层。拉 /releases，cJSON 解析，通道筛选，
 * 按版本号排序取最大（防 created_at 排序导致的降级）。
 */
#include "au_internal.h"
#include "third_party/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 从一个 release 的 cJSON 对象构造 au_release_t（不含筛选）。成功返回 1。 */
static int parse_release(const cJSON* jr, au_release_t* out) {
    memset(out, 0, sizeof *out);

    const cJSON* tag = cJSON_GetObjectItemCaseSensitive(jr, "tag_name");
    if (!cJSON_IsString(tag) || !tag->valuestring) return 0;
    if (au_version_parse(tag->valuestring, &out->ver) != AU_OK) return 0;  /* 非法 tag 跳过 */

    const cJSON* pre = cJSON_GetObjectItemCaseSensitive(jr, "prerelease");
    out->is_prerelease = cJSON_IsTrue(pre) ? 1 : 0;

    const cJSON* body = cJSON_GetObjectItemCaseSensitive(jr, "body");
    if (cJSON_IsString(body) && body->valuestring)
        out->changelog = au_strdup(body->valuestring);   /* cJSON 已反转义 */

    const cJSON* assets = cJSON_GetObjectItemCaseSensitive(jr, "assets");
    int acount = cJSON_IsArray(assets) ? cJSON_GetArraySize(assets) : 0;
    if (acount > 0) {
        out->assets = (au_asset_t*)calloc((size_t)acount, sizeof(au_asset_t));
        if (!out->assets) return 0;
        int idx = 0;
        const cJSON* ja;
        cJSON_ArrayForEach(ja, assets) {
            const cJSON* an = cJSON_GetObjectItemCaseSensitive(ja, "name");
            const cJSON* au = cJSON_GetObjectItemCaseSensitive(ja, "browser_download_url");
            const cJSON* asz = cJSON_GetObjectItemCaseSensitive(ja, "size");
            const cJSON* adg = cJSON_GetObjectItemCaseSensitive(ja, "digest");
            if (!cJSON_IsString(an) || !cJSON_IsString(au)) continue;
            au_asset_t* a = &out->assets[idx++];
            a->name = au_strdup(an->valuestring);
            a->url  = au_strdup(au->valuestring);
            a->size = cJSON_IsNumber(asz) ? (uint64_t)asz->valuedouble : 0;
            if (cJSON_IsString(adg) && adg->valuestring) {
                /* 形如 "sha256:abc..."，剥前缀。 */
                const char* p = strchr(adg->valuestring, ':');
                a->sha256 = au_strdup(p ? p + 1 : adg->valuestring);
            }
        }
        out->asset_count = (size_t)idx;
    }
    return 1;
}

/* 单个 release 的深释放（内部用；顶层 au_release_free 复用）。 */
static void free_release_contents(au_release_t* r) {
    if (!r) return;
    free(r->changelog);
    for (size_t i = 0; i < r->asset_count; i++) {
        free(r->assets[i].name);
        free(r->assets[i].url);
        free(r->assets[i].sha256);
    }
    free(r->assets);
    memset(r, 0, sizeof *r);
}

void au_release_free(au_release_t* rel) {
    if (!rel) return;
    free_release_contents(rel);
    free(rel);
}

/* 拉取 API 响应（带镜像轮换 + 重试）。 */
static au_err_t fetch_api_json(au_ctx_t* ctx, char** out, size_t* len) {
    char raw[512];
    snprintf(raw, sizeof raw,
             "https://api.github.com/repos/%s/%s/releases?per_page=100",
             ctx->owner, ctx->repo);

    size_t nmir = 0;
    wchar_t** urls = au_build_mirror_urls(ctx, raw, &nmir);
    if (!urls || nmir == 0) { free(urls); return AU_ERR_NOMEM; }

    au_err_t rc = AU_ERR_NET;
    for (size_t i = 0; i < nmir; i++) {
        if (!urls[i]) continue;
        for (int attempt = 0; attempt <= ctx->retry_max; attempt++) {
            au_http_meta meta;
            rc = au_http_get_mem(ctx, urls[i], out, len, &meta);
            if (rc == AU_OK) goto cleanup;
            if (rc == AU_ERR_RATELIMIT) { au_logf(ctx, AU_LOG_ERROR, "GitHub API rate limited"); goto cleanup; }
            au_logf(ctx, AU_LOG_WARN, "API attempt failed (mirror %zu, try %d): %s",
                    i, attempt, au_strerror(rc));
        }
    }
cleanup:
    for (size_t i = 0; i < nmir; i++) free(urls[i]);
    free(urls);
    return rc;
}

au_err_t au_select_best_from_json(au_ctx_t* ctx, const char* json, size_t len,
                                  au_release_t** out) {
    if (!ctx || !json || !out) return AU_ERR_ARG;
    *out = NULL;

    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root || !cJSON_IsArray(root)) { cJSON_Delete(root); return AU_ERR_PARSE; }

    au_release_t best; int have_best = 0;
    memset(&best, 0, sizeof best);

    const cJSON* jr;
    cJSON_ArrayForEach(jr, root) {
        /* draft 无条件剔除。 */
        const cJSON* draft = cJSON_GetObjectItemCaseSensitive(jr, "draft");
        if (cJSON_IsTrue(draft)) continue;

        au_release_t cand;
        if (!parse_release(jr, &cand)) { free_release_contents(&cand); continue; }

        /* stable 通道剔除预发布版。 */
        if (ctx->channel == AU_CHANNEL_STABLE && cand.is_prerelease) {
            free_release_contents(&cand);
            continue;
        }

        /* 按版本号取最大（不能取列表首项——GitHub 按 created_at 排序）。 */
        if (!have_best || au_version_cmp(&cand.ver, &best.ver) > 0) {
            free_release_contents(&best);
            best = cand;          /* 移交所有权 */
            have_best = 1;
        } else {
            free_release_contents(&cand);
        }
    }
    cJSON_Delete(root);

    if (!have_best) {
        au_logf(ctx, AU_LOG_WARN, "no usable release in channel");
        return AU_ERR_PARSE;   /* 没有任何可用 release */
    }

    au_release_t* heap = (au_release_t*)malloc(sizeof *heap);
    if (!heap) { free_release_contents(&best); return AU_ERR_NOMEM; }
    *heap = best;
    *out = heap;
    return AU_OK;
}

au_err_t au_fetch_latest(au_ctx_t* ctx, au_release_t** out) {
    if (!ctx || !out) return AU_ERR_ARG;
    *out = NULL;

    char* json = NULL; size_t jlen = 0;
    au_err_t rc = fetch_api_json(ctx, &json, &jlen);

    /* 正规 REST API 优先；仅在被限流时退到 github.com 网页端点兜底（见 au_web.c）。
     * 其它错误（网络/HTTP/解析）如实上报，不被兜底掩盖。 */
    if (rc == AU_ERR_RATELIMIT) {
        au_logf(ctx, AU_LOG_WARN, "REST API rate limited; falling back to web endpoint");
        free(json);
        return au_fetch_web_fallback(ctx, out);
    }
    if (rc != AU_OK) { free(json); return rc; }

    rc = au_select_best_from_json(ctx, json, jlen, out);
    free(json);
    return rc;
}

int au_compare_current(au_ctx_t* ctx, const au_release_t* rel) {
    if (!ctx || !rel) return 0;
    return au_version_cmp(&rel->ver, &ctx->current);
}

const au_asset_t* au_pick_asset(const au_release_t* rel, const char* pattern) {
    if (!rel || !pattern) return NULL;
    const au_asset_t* hit = NULL;
    for (size_t i = 0; i < rel->asset_count; i++) {
        if (au_glob_match(pattern, rel->assets[i].name)) {
            if (hit) return NULL;      /* 多重匹配：宁可返回 NULL 也不猜 */
            hit = &rel->assets[i];
        }
    }
    return hit;
}
