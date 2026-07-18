/*
 * au_version.c — 版本解析与比较。把 semver.c 完全封装在实现文件内，
 * 使 au_version_t 保持 POD（无堆、可自由拷贝）。换掉 semver.c 不影响公开 API。
 */
#include "au_internal.h"
#include "third_party/semver.h"
#include <stdlib.h>
#include <string.h>

/* 跳过前导 v/V。 */
static const char* skip_v(const char* tag) {
    if (tag && (tag[0] == 'v' || tag[0] == 'V')) return tag + 1;
    return tag;
}

au_err_t au_version_parse(const char* tag, au_version_t* out) {
    if (!tag || !out) return AU_ERR_ARG;
    memset(out, 0, sizeof *out);

    size_t rawlen = strlen(tag);
    if (rawlen == 0 || rawlen >= sizeof out->raw) return AU_ERR_ARG;  /* 不静默截断 */
    memcpy(out->raw, tag, rawlen + 1);

    const char* core = skip_v(tag);
    semver_t sv;
    memset(&sv, 0, sizeof sv);
    if (semver_parse(core, &sv) != 0) {
        semver_free(&sv);
        return AU_ERR_ARG;
    }
    out->major = sv.major;
    out->minor = sv.minor;
    out->patch = sv.patch;
    if (sv.prerelease) {
        strncpy(out->prerelease, sv.prerelease, sizeof out->prerelease - 1);
        out->prerelease[sizeof out->prerelease - 1] = '\0';
    }
    semver_free(&sv);
    return AU_OK;
}

int au_version_cmp(const au_version_t* a, const au_version_t* b) {
    if (!a || !b) return 0;
    semver_t sa, sb;
    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    /* 用 raw（去 v）走 semver.c，以获得完整的 prerelease 优先级规则。 */
    int ra = semver_parse(skip_v(a->raw), &sa);
    int rb = semver_parse(skip_v(b->raw), &sb);
    int result;
    if (ra != 0 || rb != 0) {
        /* 任一无法解析：按等价处理（安全默认——不触发动作）。 */
        result = 0;
    } else {
        result = semver_compare(sa, sb);
    }
    semver_free(&sa);
    semver_free(&sb);
    return result;
}
