/*
 * config.h —— .ini 数据模型与解析。纯逻辑，无 Win32 依赖（便于单测）。
 * 解析用 inih（third_party/ini.c）；UTF-8 按字节透明，BOM 由 inih 剥离。
 * 方向数字：8上 2下 4左 6右 / 7左上 9右上 1左下 3右下。
 */
#ifndef HUA_CONFIG_H
#define HUA_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CFG_TRIGGER_RIGHT = 0,
    CFG_TRIGGER_MIDDLE,
    CFG_TRIGGER_X1,
    CFG_TRIGGER_X2,
} CfgTrigger;

typedef enum {
    CFG_FILTER_BLACKLIST = 0,
    CFG_FILTER_WHITELIST,
} CfgFilterMode;

#define CFG_MAX_KEY          16
#define CFG_MAX_ACTION       256
#define CFG_MAX_GESTURES     128
#define CFG_MAX_APPS         64
#define CFG_MAX_APP_GESTURES 64
#define CFG_MAX_EXE          64

typedef struct {
    char key[CFG_MAX_KEY];       /* 方向串，如 "26" */
    char action[CFG_MAX_ACTION]; /* 动作值，如 "key:ctrl+w" */
} Gesture;

typedef struct {
    char    name[CFG_MAX_EXE];   /* exe 名，已小写 */
    bool    enabled;
    Gesture gestures[CFG_MAX_APP_GESTURES];
    size_t  gesture_count;
} AppConfig;

typedef struct {
    /* [General] —— 触发与识别 */
    CfgTrigger    trigger;
    int           trigger_distance;  /* 按下后移动多远才开始手势（px） */
    int           min_distance;      /* 方向分段阈值（识别灵敏度，px） */
    int           step_distance;     /* 采点最小间隔（去抖，px） */
    int           tolerance;         /* 方向串匹配最大编辑距离（0=精确） */
    int           pause_timeout;     /* 久不动则取消手势（ms） */
    CfgFilterMode filter_mode;
    bool          disable_on_fullscreen;
    bool          auto_start;
    bool          restore_event;     /* 未形成可见轨迹的识别失败是否补发原生按键 */

    /* [General] —— 浮层外观（overlay/M5 消费） */
    bool          show_trail;
    bool          show_action_name;
    bool          trail_arrow;       /* 轨迹末端画方向箭头 */
    bool          random_color;      /* 轨迹随机颜色 */
    unsigned      trail_color;       /* 命中动作名 OSD 描边色 0xRRGGBB */
    unsigned      fail_color;        /* 未命中「手势无动作」OSD 描边色 0xRRGGBB */
    int           trail_width;
    int           trail_max_length;  /* 轨迹绘制长度上限（px，0=不限）；仅影响绘制，不影响识别 */
    int           text_size;         /* 动作名 OSD 字号 */
    int           text_position;     /* OSD 距屏幕底部高度（px） */
    unsigned      text_fill_color;   /* OSD 文字镂空填充色 0xRRGGBB（默认白） */
    int           text_outline_width;/* OSD 文字描边宽度（px） */
    int           text_letter_spacing;/* OSD 文字字间距（px） */

    /* [Gestures] 全局 */
    Gesture gestures[CFG_MAX_GESTURES];
    size_t  gesture_count;

    /* [App:xxx] */
    AppConfig apps[CFG_MAX_APPS];
    size_t    app_count;
} Config;

/* 以推荐默认值初始化。 */
void config_set_defaults(Config *c);

/* 解析 UTF-8 ini 文本到 c（内部先 set_defaults）。坏行跳过，仍返回 true。
 * 文件读取由调用方负责（保持本模块纯逻辑、可脱离系统单测）。 */
bool config_parse_string(Config *c, const char *text);

/* 全局手势查找：命中返回 action 指针，否则 NULL。 */
const char *config_lookup_global(const Config *c, const char *seq);

/* 按 exe 名查 app 配置（小写匹配）；无则 NULL。 */
const AppConfig *config_find_app(const Config *c, const char *exe_lower);

/* app 内手势查找。 */
const char *config_lookup_app(const AppConfig *app, const char *seq);

/*
 * 该前台程序当前是否启用手势（门控，触发前判定）。
 * 规则：全屏且 DisableOnFullscreen → 否；whitelist 且无 [App:exe] → 否；
 *       [App:exe].enabled==false → 否；否则 是。
 * exe_lower 可为 NULL（取不到前台 exe）。
 */
bool config_app_enabled(const Config *c, const char *exe_lower, bool is_fullscreen);

/*
 * 解析手势动作：程序覆盖 > 全局默认，均按 Tolerance 匹配。
 * 命中返回 action 指针，否则 NULL。exe_lower 可为 NULL（只查全局）。
 */
const char *config_resolve(const Config *c, const char *exe_lower, const char *seq);

#endif /* HUA_CONFIG_H */
