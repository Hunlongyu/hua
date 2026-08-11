/*
 * config.c —— 见 config.h。解析用 inih 的 SAX 回调，边解析边建模型。
 */
#include "config.h"
#include "ini.h"
#include "recognizer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

static bool cieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++; b++;
    }
    return *a == *b;
}

/* s 是否以 prefix（长 n）开头，大小写不敏感。纯 ASCII、locale 无关，与 cieq 同源。 */
static bool ci_prefix(const char *s, const char *prefix, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!s[i] || tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void copy_lower(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

/* 记下首个出问题的键名，供调用方在日志里指名道姓。只留第一个就够定位。 */
static void record_issue(Config *c, const char *name)
{
    if (c->diag.first_issue[0] == '\0' && name)
        copy_str(c->diag.first_issue, sizeof(c->diag.first_issue), name);
}

/*
 * 布尔值解析。无法识别时回落到该字段的**文档默认值** def，并计入诊断。
 *
 * 不能一律返回 false：受害字段（DisableOnFullscreen / RestoreEvent / ShowTrail /
 * ShowActionName / TrailArrow / [App:].Enabled）默认全是 true，于是 `ShowTrail = 真`
 * 或 `DisableOnFullscreen = ture` 这类手误会把功能静默关掉——后者尤其糟糕，全屏门控
 * 一关，手势就会在全屏游戏里误触发，而那正是这个开关存在的全部理由。
 * 数值字段早有同样的回落机制（见 fallback_clamp：`MinDistance = 二十` → 回落 20）；
 * 中文用户误填 `真`/`是` 与误填 `二十` 是同一类错误，理应同等对待。
 */
static bool parse_bool(Config *c, const char *name, const char *v, bool def)
{
    if (cieq(v, "true")  || cieq(v, "1") || cieq(v, "yes") || cieq(v, "on"))
        return true;
    if (cieq(v, "false") || cieq(v, "0") || cieq(v, "no")  || cieq(v, "off"))
        return false;
    c->diag.bad_values++;
    record_issue(c, name);
    return def;
}

static CfgTrigger parse_trigger(const char *v)
{
    if (cieq(v, "middle")) return CFG_TRIGGER_MIDDLE;
    if (cieq(v, "x1"))     return CFG_TRIGGER_X1;
    if (cieq(v, "x2"))     return CFG_TRIGGER_X2;
    return CFG_TRIGGER_RIGHT;
}

static CfgFilterMode parse_filter(const char *v)
{
    if (cieq(v, "whitelist")) return CFG_FILTER_WHITELIST;
    return CFG_FILTER_BLACKLIST;
}

static CfgLogLevel parse_log_level(Config *c, const char *name, const char *v)
{
    if (cieq(v, "off"))   return CFG_LOG_OFF;
    if (cieq(v, "error")) return CFG_LOG_ERROR;
    if (cieq(v, "warn") || cieq(v, "warning")) return CFG_LOG_WARN;
    if (cieq(v, "info"))  return CFG_LOG_INFO;
    c->diag.bad_values++;
    record_issue(c, name);
    return CFG_LOG_WARN;
}

/* ---------------- 默认值 ---------------- */

void config_set_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->trigger               = CFG_TRIGGER_RIGHT;
    c->trigger_distance      = 5;
    c->min_distance          = 20;
    c->step_distance         = 12;
    c->match_score           = CFG_DEFAULT_MATCH_SCORE;
    c->ambiguity_margin      = CFG_DEFAULT_AMBIGUITY_MARGIN;
    c->pause_timeout         = 1000;
    c->filter_mode           = CFG_FILTER_BLACKLIST;
    c->disable_on_fullscreen = true;
    c->auto_start            = false;
    c->restore_event         = true;
    c->log_enabled           = true;
    c->log_level             = CFG_LOG_WARN;
    c->log_max_size_mb       = 10;
    c->log_retention_days    = 2;
    c->show_trail            = true;
    c->show_action_name      = true;
    c->trail_arrow           = true;
    c->random_color          = false;
    c->trail_color           = 0x00A0FF;
    c->fail_color            = 0x666666;
    c->trail_width           = 3;
    c->trail_max_length      = 2500;           /* 轨迹绘制长度上限（px，0=不限） */
    c->text_size             = 26;
    c->text_position         = 150;
    c->text_fill_color       = 0xFFFFFF;       /* 字芯默认白色镂空 */
    c->text_outline_width    = 3;              /* 描边宽度（px） */
    c->text_letter_spacing   = 4;              /* 字间距（px） */
    c->update_enabled        = true;
    c->update_auto_check     = true;
    c->update_channel        = CFG_CHANNEL_STABLE;
    c->gesture_count         = 0;
    c->app_count             = 0;
    memset(&c->diag, 0, sizeof(c->diag));
}

/* ---------------- 建模：各节 ---------------- */

static bool normalize_gesture_key(Config *c, const char *key,
                                  char normalized[CFG_MAX_KEY])
{
    if (rec_normalize_template(key, normalized, CFG_MAX_KEY) != 0)
        return true;
    c->diag.invalid_gestures++;
    record_issue(c, key);
    return false;
}

static void add_global_gesture(Config *c, const char *key, const char *val)
{
    char normalized[CFG_MAX_KEY];
    if (!normalize_gesture_key(c, key, normalized))
        return;

    for (size_t i = 0; i < c->gesture_count; i++) {
        if (strcmp(c->gestures[i].key, normalized) == 0) {
            copy_str(c->gestures[i].action, CFG_MAX_ACTION, val);
            c->diag.duplicate_gestures++;
            record_issue(c, key);
            return; /* 与常见 ini 语义一致：同一标准模板后定义覆盖前定义 */
        }
    }
    if (c->gesture_count >= CFG_MAX_GESTURES)
    {
        c->diag.dropped++;
        record_issue(c, key);
        return;
    }
    Gesture *g = &c->gestures[c->gesture_count++];
    copy_str(g->key, CFG_MAX_KEY, normalized);
    copy_str(g->action, CFG_MAX_ACTION, val);
}

/* 找到或新建 app（按小写 exe 名）。满则返回 NULL。 */
static AppConfig *find_or_create_app(Config *c, const char *exe)
{
    char lower[CFG_MAX_EXE];
    copy_lower(lower, CFG_MAX_EXE, exe);

    for (size_t i = 0; i < c->app_count; i++)
        if (strcmp(c->apps[i].name, lower) == 0)
            return &c->apps[i];

    if (c->app_count >= CFG_MAX_APPS)
        return NULL;
    AppConfig *a = &c->apps[c->app_count++];
    memset(a, 0, sizeof(*a));
    copy_str(a->name, CFG_MAX_EXE, lower);
    a->enabled = true;   /* 默认启用；除非显式 Enabled=false */
    a->gesture_count = 0;
    return a;
}

static void add_app_gesture(Config *c, AppConfig *a, const char *key, const char *val)
{
    char normalized[CFG_MAX_KEY];
    if (!normalize_gesture_key(c, key, normalized))
        return;

    for (size_t i = 0; i < a->gesture_count; i++) {
        if (strcmp(a->gestures[i].key, normalized) == 0) {
            copy_str(a->gestures[i].action, CFG_MAX_ACTION, val);
            c->diag.duplicate_gestures++;
            record_issue(c, key);
            return;
        }
    }
    if (a->gesture_count >= CFG_MAX_APP_GESTURES)
    {
        c->diag.dropped++;
        record_issue(c, key);
        return;
    }
    Gesture *g = &a->gestures[a->gesture_count++];
    copy_str(g->key, CFG_MAX_KEY, normalized);
    copy_str(g->action, CFG_MAX_ACTION, val);
}

/* ---------------- inih 回调 ---------------- */

static int handler(void *user, const char *section, const char *name,
                   const char *value)
{
    Config *c = (Config *)user;

    if (cieq(section, "General")) {
        if      (cieq(name, "Trigger"))             c->trigger = parse_trigger(value);
        else if (cieq(name, "TriggerDistance"))     c->trigger_distance = atoi(value);
        else if (cieq(name, "MinDistance"))         c->min_distance = atoi(value);
        else if (cieq(name, "StepDistance"))        c->step_distance = atoi(value);
        else if (cieq(name, "MatchScore"))          c->match_score = atoi(value);
        else if (cieq(name, "AmbiguityMargin"))     c->ambiguity_margin = atoi(value);
        else if (cieq(name, "PauseTimeout"))        c->pause_timeout = atoi(value);
        else if (cieq(name, "FilterMode"))          c->filter_mode = parse_filter(value);
        /* 第 4 个实参是该字段的文档默认值，须与 config_set_defaults 保持一致。 */
        else if (cieq(name, "DisableOnFullscreen")) c->disable_on_fullscreen = parse_bool(c, name, value, true);
        else if (cieq(name, "AutoStart"))           c->auto_start = parse_bool(c, name, value, false);
        else if (cieq(name, "RestoreEvent"))        c->restore_event = parse_bool(c, name, value, true);
        else if (cieq(name, "LogEnabled"))          c->log_enabled = parse_bool(c, name, value, true);
        else if (cieq(name, "LogLevel"))            c->log_level = parse_log_level(c, name, value);
        else if (cieq(name, "LogMaxSizeMB"))        c->log_max_size_mb = atoi(value);
        else if (cieq(name, "LogRetentionDays"))    c->log_retention_days = atoi(value);
        else if (cieq(name, "ShowTrail"))           c->show_trail = parse_bool(c, name, value, true);
        else if (cieq(name, "ShowActionName"))      c->show_action_name = parse_bool(c, name, value, true);
        else if (cieq(name, "TrailArrow"))          c->trail_arrow = parse_bool(c, name, value, true);
        else if (cieq(name, "RandomColor"))         c->random_color = parse_bool(c, name, value, false);
        else if (cieq(name, "TrailColor"))          c->trail_color = (unsigned)strtoul(value, NULL, 16);
        else if (cieq(name, "FailColor"))           c->fail_color = (unsigned)strtoul(value, NULL, 16);
        else if (cieq(name, "TrailWidth"))          c->trail_width = atoi(value);
        else if (cieq(name, "TrailMaxLength"))      c->trail_max_length = atoi(value);
        else if (cieq(name, "TextSize"))            c->text_size = atoi(value);
        else if (cieq(name, "TextPosition"))        c->text_position = atoi(value);
        else if (cieq(name, "TextFillColor"))       c->text_fill_color = (unsigned)strtoul(value, NULL, 16);
        else if (cieq(name, "TextOutlineWidth"))    c->text_outline_width = atoi(value);
        else if (cieq(name, "TextLetterSpacing"))   c->text_letter_spacing = atoi(value);
        else {
            /* 拼错的键（TrailWidth → TrailWith）此前被静默忽略：设置永远不生效，
             * 日志却一片祥和。inih 只报「行本身语法坏」，键名拼错它是合法行。 */
            c->diag.unknown_keys++;
            record_issue(c, name);
        }
        return 1;
    }

    if (cieq(section, "Update")) {
        if      (cieq(name, "Enabled"))   c->update_enabled    = parse_bool(c, name, value, true);
        else if (cieq(name, "AutoCheck")) c->update_auto_check = parse_bool(c, name, value, true);
        else if (cieq(name, "Channel"))
            c->update_channel = cieq(value, "beta") ? CFG_CHANNEL_BETA : CFG_CHANNEL_STABLE;
        else { c->diag.unknown_keys++; record_issue(c, name); }
        return 1;
    }

    if (cieq(section, "Gestures")) {
        add_global_gesture(c, name, value);
        return 1;
    }

    /* [App:xxx] —— 前缀大小写不敏感。此前只硬编码了 "App:"/"app:" 两种写法，
     * [APP:chrome.exe] 会被静默忽略（既无诊断、per-app 覆盖也完全失效），
     * 与本文件其余各处一律用 cieq 的约定也不一致。 */
    if (ci_prefix(section, "app:", 4)) {
        AppConfig *a = find_or_create_app(c, section + 4);
        if (!a) {
            /* [App:] 节数已达上限：整节被丢弃。若该节本意是 Enabled=false，
             * 用户会看到「游戏里还是会误触发手势」，且毫无线索。 */
            c->diag.dropped++;
            record_issue(c, section);
        } else if (cieq(name, "Enabled")) {
            a->enabled = parse_bool(c, name, value, true);
        } else
            add_app_gesture(c, a, name, value);
        return 1;
    }

    return 1;   /* 未知节：忽略但不报错 */
}

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * 用于「必须为正」的字段：非正值（含 atoi 对垃圾输入返回的 0）回落到**文档默认值**，
 * 而不是区间下界。区别很关键：`MinDistance = 二十` 时 atoi 得 0，回落到 20 才能继续
 * 正常工作；若夹成下界 1，则每次像素抖动都会被切成一个方向段，所有手势都将失效。
 */
static int fallback_clamp(int v, int def, int lo, int hi)
{
    if (v <= 0)
        return def;
    return clamp_int(v, lo, hi);
}

/*
 * 校正 ini 数值到合理区间。ini 全是不可信输入：atoi 对垃圾输入返回 0，对超长
 * 数字是 UB。不校验会导致有符号溢出（MinDistance 等会被平方，而 long 在 Windows
 * 恒为 32 位）与荒谬的浮层布局算术。
 */
static void config_clamp(Config *c)
{
    /* 必须为正的字段：非法/缺失 → 文档默认值（与 config_set_defaults 保持一致）。 */
    c->trigger_distance = fallback_clamp(c->trigger_distance, 5,  1, 10000);
    c->min_distance     = fallback_clamp(c->min_distance,     20, 1, 10000);
    c->step_distance    = fallback_clamp(c->step_distance,    12, 1, 10000);
    c->trail_width      = fallback_clamp(c->trail_width,      3,  1, 200);
    c->text_size        = fallback_clamp(c->text_size,        26, 1, 500);
    c->log_max_size_mb  = fallback_clamp(c->log_max_size_mb,  10, 1, 1024);
    c->log_retention_days = fallback_clamp(c->log_retention_days, 2, 1, 3650);

    /* MatchScore 必须为正；AmbiguityMargin=0 则明确关闭歧义拒识。 */
    c->match_score        = fallback_clamp(c->match_score,
                                            CFG_DEFAULT_MATCH_SCORE, 1, 100);
    c->ambiguity_margin   = clamp_int(c->ambiguity_margin, 0, 30);

    /* 0 是合法语义的字段：trail_max_length=0 不限、text_outline_width=0 不描边、
     * pause_timeout=0 不超时。故只夹不回落。 */
    c->pause_timeout       = clamp_int(c->pause_timeout,       0, 600000);
    c->trail_max_length    = clamp_int(c->trail_max_length,    0, 1000000);
    c->text_outline_width  = clamp_int(c->text_outline_width,  0, 100);
    c->text_letter_spacing = clamp_int(c->text_letter_spacing, 0, 500);
    /* 负值有意义：把 OSD 放到该屏底边以下（上下堆叠的多屏布局）。 */
    c->text_position       = clamp_int(c->text_position,   -10000, 10000);
}

bool config_parse_string_ex(Config *c, const char *text, int *out_bad_line)
{
    config_set_defaults(c);
    int r = ini_parse_string(text, handler, c);
    config_clamp(c);
    if (out_bad_line)
        *out_bad_line = (r > 0) ? r : 0;
    /* r>0 = 某行解析错（坏行已跳过）；-2 = 内存不足。除内存外都视为成功。 */
    return r != -2;
}

bool config_parse_string(Config *c, const char *text)
{
    return config_parse_string_ex(c, text, NULL);
}

/* ---------------- 查找 ---------------- */

const char *config_lookup_global(const Config *c, const char *seq)
{
    if (!c || !seq)
        return NULL;
    for (size_t i = 0; i < c->gesture_count; i++)
        if (strcmp(c->gestures[i].key, seq) == 0)
            return c->gestures[i].action;
    return NULL;
}

const AppConfig *config_find_app(const Config *c, const char *exe_lower)
{
    if (!c || !exe_lower)
        return NULL;
    for (size_t i = 0; i < c->app_count; i++)
        if (strcmp(c->apps[i].name, exe_lower) == 0)
            return &c->apps[i];
    return NULL;
}

const char *config_lookup_app(const AppConfig *app, const char *seq)
{
    if (!app || !seq)
        return NULL;
    for (size_t i = 0; i < app->gesture_count; i++)
        if (strcmp(app->gestures[i].key, seq) == 0)
            return app->gestures[i].action;
    return NULL;
}

bool config_app_enabled(const Config *c, const char *exe_lower, bool is_fullscreen)
{
    if (c->disable_on_fullscreen && is_fullscreen)
        return false;
    const AppConfig *app = exe_lower ? config_find_app(c, exe_lower) : NULL;
    if (c->filter_mode == CFG_FILTER_WHITELIST && !app)
        return false;
    if (app && !app->enabled)
        return false;
    return true;
}

/* cmd:none = 显式「无动作」。主要用于 per-app 屏蔽某个全局手势
 * （如 39=key:f5 在 PPT 里会触发放映，可用 [App:powerpnt.exe] 39=cmd:none 挡掉）。
 * 归一到「未匹配」语义：调用方拿到 NULL，浮层照常提示「手势无动作」。 */
static bool is_none_action(const char *a)
{
    return a && (cieq(a, "cmd:none") || cieq(a, "none"));
}

const char *config_resolve(const Config *c, const char *exe_lower, const char *seq)
{
    if (!c || !seq || !seq[0])
        return NULL;
    const AppConfig *app = exe_lower ? config_find_app(c, exe_lower) : NULL;
    if (app) {
        const char *a = config_lookup_app(app, seq);
        /* 程序覆盖优先；命中 cmd:none 必须就地返回 NULL，绝不能回落到全局映射，
         * 否则屏蔽不生效。 */
        if (a)
            return is_none_action(a) ? NULL : a;
    }
    const char *g = config_lookup_global(c, seq);
    return is_none_action(g) ? NULL : g;
}

static bool gesture_key_seen(const Gesture *const *items, size_t count, const char *key)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i]->key, key) == 0)
            return true;
    }
    return false;
}

const char *config_resolve_path(const Config *c, const char *exe_lower,
                                const Pt *pts, size_t n,
                                char *out_key, size_t out_key_cap,
                                RecMatchResult *result)
{
    RecMatchResult local = {-1, 0, 0};
    if (!result)
        result = &local;
    *result = local;
    if (out_key && out_key_cap)
        out_key[0] = '\0';
    if (!c)
        return NULL;

    /* 建立“当前程序真正可见”的手势表：app 同 key 覆盖全局，而非先让 app 中任意
     * 模糊候选抢占。这样所有有效手势在同一个评分空间公平竞争。 */
    const Gesture *items[CFG_MAX_APP_GESTURES + CFG_MAX_GESTURES];
    const char *keys[CFG_MAX_APP_GESTURES + CFG_MAX_GESTURES];
    size_t count = 0;
    const AppConfig *app = exe_lower ? config_find_app(c, exe_lower) : NULL;

    if (app) {
        for (size_t i = 0; i < app->gesture_count; i++) {
            if (!gesture_key_seen(items, count, app->gestures[i].key))
                items[count++] = &app->gestures[i];
        }
    }
    for (size_t i = 0; i < c->gesture_count; i++) {
        if (!gesture_key_seen(items, count, c->gestures[i].key))
            items[count++] = &c->gestures[i];
    }
    for (size_t i = 0; i < count; i++)
        keys[i] = items[i]->key;

    int index = rec_match_path(pts, n, c->min_distance, keys, count,
                               c->match_score, c->ambiguity_margin, result);
    if (index < 0) {
        if (out_key && out_key_cap)
            rec_encode(pts, n, c->min_distance, out_key, out_key_cap);
        return NULL;
    }

    const Gesture *matched = items[index];
    if (out_key && out_key_cap) {
        strncpy(out_key, matched->key, out_key_cap - 1);
        out_key[out_key_cap - 1] = '\0';
    }
    return is_none_action(matched->action) ? NULL : matched->action;
}
