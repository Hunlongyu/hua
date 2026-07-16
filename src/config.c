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

static bool parse_bool(const char *v)
{
    return cieq(v, "true") || cieq(v, "1") || cieq(v, "yes") || cieq(v, "on");
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

/* ---------------- 默认值 ---------------- */

void config_set_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->trigger               = CFG_TRIGGER_RIGHT;
    c->trigger_distance      = 5;
    c->min_distance          = 20;
    c->step_distance         = 12;
    c->tolerance             = 0;              /* 精确匹配（我们的默认） */
    c->pause_timeout         = 1000;
    c->filter_mode           = CFG_FILTER_BLACKLIST;
    c->disable_on_fullscreen = true;
    c->auto_start            = false;
    c->restore_event         = true;
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
    c->gesture_count         = 0;
    c->app_count             = 0;
}

/* ---------------- 建模：各节 ---------------- */

static void add_global_gesture(Config *c, const char *key, const char *val)
{
    if (c->gesture_count >= CFG_MAX_GESTURES)
        return;
    Gesture *g = &c->gestures[c->gesture_count++];
    copy_str(g->key, CFG_MAX_KEY, key);
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

static void add_app_gesture(AppConfig *a, const char *key, const char *val)
{
    if (a->gesture_count >= CFG_MAX_APP_GESTURES)
        return;
    Gesture *g = &a->gestures[a->gesture_count++];
    copy_str(g->key, CFG_MAX_KEY, key);
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
        else if (cieq(name, "Tolerance"))           c->tolerance = atoi(value);
        else if (cieq(name, "PauseTimeout"))        c->pause_timeout = atoi(value);
        else if (cieq(name, "FilterMode"))          c->filter_mode = parse_filter(value);
        else if (cieq(name, "DisableOnFullscreen")) c->disable_on_fullscreen = parse_bool(value);
        else if (cieq(name, "AutoStart"))           c->auto_start = parse_bool(value);
        else if (cieq(name, "RestoreEvent"))        c->restore_event = parse_bool(value);
        else if (cieq(name, "ShowTrail"))           c->show_trail = parse_bool(value);
        else if (cieq(name, "ShowActionName"))      c->show_action_name = parse_bool(value);
        else if (cieq(name, "TrailArrow"))          c->trail_arrow = parse_bool(value);
        else if (cieq(name, "RandomColor"))         c->random_color = parse_bool(value);
        else if (cieq(name, "TrailColor"))          c->trail_color = (unsigned)strtoul(value, NULL, 16);
        else if (cieq(name, "FailColor"))           c->fail_color = (unsigned)strtoul(value, NULL, 16);
        else if (cieq(name, "TrailWidth"))          c->trail_width = atoi(value);
        else if (cieq(name, "TrailMaxLength"))      c->trail_max_length = atoi(value);
        else if (cieq(name, "TextSize"))            c->text_size = atoi(value);
        else if (cieq(name, "TextPosition"))        c->text_position = atoi(value);
        else if (cieq(name, "TextFillColor"))       c->text_fill_color = (unsigned)strtoul(value, NULL, 16);
        else if (cieq(name, "TextOutlineWidth"))    c->text_outline_width = atoi(value);
        else if (cieq(name, "TextLetterSpacing"))   c->text_letter_spacing = atoi(value);
        return 1;
    }

    if (cieq(section, "Gestures")) {
        add_global_gesture(c, name, value);
        return 1;
    }

    /* [App:xxx] */
    if (strncmp(section, "App:", 4) == 0 || strncmp(section, "app:", 4) == 0) {
        AppConfig *a = find_or_create_app(c, section + 4);
        if (a) {
            if (cieq(name, "Enabled"))
                a->enabled = parse_bool(value);
            else
                add_app_gesture(a, name, value);
        }
        return 1;
    }

    return 1;   /* 未知节：忽略但不报错 */
}

bool config_parse_string(Config *c, const char *text)
{
    config_set_defaults(c);
    int r = ini_parse_string(text, handler, c);
    /* r>0 = 某行解析错（坏行已跳过）；-2 = 内存不足。除内存外都视为成功。 */
    return r != -2;
}

/* ---------------- 查找 ---------------- */

const char *config_lookup_global(const Config *c, const char *seq)
{
    for (size_t i = 0; i < c->gesture_count; i++)
        if (strcmp(c->gestures[i].key, seq) == 0)
            return c->gestures[i].action;
    return NULL;
}

const AppConfig *config_find_app(const Config *c, const char *exe_lower)
{
    for (size_t i = 0; i < c->app_count; i++)
        if (strcmp(c->apps[i].name, exe_lower) == 0)
            return &c->apps[i];
    return NULL;
}

const char *config_lookup_app(const AppConfig *app, const char *seq)
{
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

/* 在一组 Gesture 上用 rec_match（按 tolerance）匹配，命中返回 action。 */
static const char *match_gestures(const Gesture *g, size_t n, const char *seq,
                                  int tolerance)
{
    const char *keys[CFG_MAX_GESTURES];
    if (n > CFG_MAX_GESTURES)
        n = CFG_MAX_GESTURES;
    for (size_t i = 0; i < n; i++)
        keys[i] = g[i].key;
    int idx = rec_match(seq, keys, n, tolerance);
    return idx >= 0 ? g[idx].action : NULL;
}

const char *config_resolve(const Config *c, const char *exe_lower, const char *seq)
{
    const AppConfig *app = exe_lower ? config_find_app(c, exe_lower) : NULL;
    if (app) {
        const char *a = match_gestures(app->gestures, app->gesture_count,
                                       seq, c->tolerance);
        if (a)
            return a;   /* 程序覆盖优先 */
    }
    return match_gestures(c->gestures, c->gesture_count, seq, c->tolerance);
}
