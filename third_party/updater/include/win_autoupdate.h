/*
 * win_autoupdate.h — Windows 纯 C GitHub Release 自动更新库
 *
 * 公开头文件，调用方只需 include 这一个文件。全部 API 使用 UTF-8 char*，
 * 内部自行转换为 Win32 的宽字符。库本身不产生任何输出，进度与日志一律通过
 * 回调交给调用方，因此可嵌入控制台程序、GUI 程序或托盘常驻程序。
 *
 * 设计文档见 docs/design.md。
 *
 * 链接：winhttp.lib bcrypt.lib
 * MIT License.
 */
#ifndef WIN_AUTOUPDATE_H
#define WIN_AUTOUPDATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================= 错误码 */
typedef enum {
    AU_OK = 0,
    AU_ERR_ARG,           /* 参数非法 */
    AU_ERR_NOMEM,
    AU_ERR_NET,           /* 连接/DNS/超时，且所有镜像均已失败 */
    AU_ERR_HTTP,          /* 非 2xx 响应（且非限流） */
    AU_ERR_RATELIMIT,     /* 403 且 X-RateLimit-Remaining: 0 */
    AU_ERR_PARSE,         /* JSON 结构不符预期 */
    AU_ERR_NO_ASSET,      /* asset_pattern 无匹配项 */
    AU_ERR_IO,            /* 文件读写失败 */
    AU_ERR_HASH,          /* SHA-256 与 API 提供的 digest 不符 */
    AU_ERR_EXTRACT,       /* tar.exe 解压失败 */
    AU_ERR_NOT_WRITABLE,  /* 安装目录不可写，需管理员权限 */
    AU_ERR_REPLACE,       /* 文件替换失败（已尽力回滚） */
    AU_ERR_LAUNCH,        /* 启动新版进程 / 安装器失败 */
    AU_ERR_CANCELED,
    AU_ERR_STATE          /* 内部状态错误（如空指针上下文） */
} au_err_t;

/* 返回错误码的静态可读串（英文短语，供日志用）。永不返回 NULL。 */
const char* au_strerror(au_err_t e);

/* ============================================================= 版本
 * POD：可自由拷贝、按值传递、无需释放。semver.c 的堆分配不会渗透到这里。 */
typedef struct {
    int  major, minor, patch;
    char prerelease[32];   /* "beta.2"；空串表示正式版 */
    char raw[64];          /* 原始 tag，如 "v1.2.0-beta.2" */
} au_version_t;

/* 解析 "v1.2.3" / "1.2.3" / "v1.2.3-beta.1" / "v1.2.3+build" 到 out。
 * 前导 v/V 可有可无。超长 tag（>=64）或无法解析返回 AU_ERR_ARG。 */
au_err_t au_version_parse(const char* tag, au_version_t* out);

/* semver 2.0 优先级：<0 表示 a 旧于 b；0 等价；>0 a 新于 b。忽略 +build。 */
int au_version_cmp(const au_version_t* a, const au_version_t* b);

/* ============================================================= 资产 / 发布 */
typedef struct {
    char*    name;      /* "hua-v1.2.3-x64.exe" */
    char*    url;       /* browser_download_url */
    char*    sha256;    /* 来自 API digest 字段；老 release 可能为 NULL */
    uint64_t size;
} au_asset_t;

typedef struct {
    au_version_t ver;
    char*        changelog;    /* 已反转义的 UTF-8 文本，可能为 NULL */
    int          is_prerelease;
    au_asset_t*  assets;
    size_t       asset_count;
} au_release_t;

void au_release_free(au_release_t* rel);

/* ============================================================= 回调 */

/* 在 au_download() 内部、调用方线程上被反复调用。
 * total 为总字节数；服务端未返回 Content-Length 时为 0（不要据此算百分比）。
 * 返回 0 继续，返回非 0 请求取消（au_download 将返回 AU_ERR_CANCELED）。 */
typedef int (*au_progress_cb)(uint64_t total, uint64_t done, void* userdata);

typedef enum { AU_LOG_DEBUG, AU_LOG_INFO, AU_LOG_WARN, AU_LOG_ERROR } au_log_level_t;
typedef void (*au_log_cb)(au_log_level_t lv, const char* msg, void* userdata);

/* ============================================================= 配置 */

/* 更新模式：
 *  EXE       —— 下载单文件 exe，原子 rename 替换自身，重启。
 *  ZIP       —— 下载 zip，tar.exe 解压覆盖安装目录（zip_exclude 保护的除外），重启。
 *  INSTALLER —— 下载安装器 exe（NSIS/Inno 等），按 installer_silent 决定静默或交互，
 *               启动后本进程退出，由安装器完成替换与拉起新版。 */
typedef enum { AU_MODE_EXE = 1, AU_MODE_ZIP = 2, AU_MODE_INSTALLER = 3 } au_mode_t;

typedef enum { AU_CHANNEL_STABLE = 0, AU_CHANNEL_BETA = 1 } au_channel_t;

typedef struct {
    /* ---- 必填 ---- */
    const char*  owner;             /* "Hunlongyu" */
    const char*  repo;              /* "hua" */
    const char*  current_version;   /* "v1.2.3" 或 "1.2.3" */
    const char*  asset_pattern;     /* glob，如 "hua-*-x64.exe"；只支持 * 与 ? */
    au_mode_t    mode;

    /* ---- 可选，0/NULL 取默认 ---- */
    au_channel_t channel;           /* 默认 AU_CHANNEL_STABLE */
    const char*  user_agent;        /* 默认 "WinAutoUpdate/1.0"；缺 UA GitHub 返回 403 */
    const char*  token;             /* 默认匿名（60 次/小时/IP） */

    /* 镜像模板数组，{url} 为完整原始 URL 的占位符，逐个尝试。
     * 例：{ "{url}", "https://gh-proxy.com/{url}" }。NULL 用内置默认。 */
    const char* const* mirrors;
    size_t       mirror_count;

    int          timeout_ms;        /* 默认 30000 */
    int          retry_max;         /* 每个镜像的重试次数，默认 2 */

    /* ---- ZIP 模式专用 ---- */
    /* 覆盖时跳过的文件名（basename，大小写不敏感），仅当目标已存在时跳过。
     * 典型：用户配置文件。NULL 表示全部覆盖。 */
    const char* const* zip_exclude;
    size_t       zip_exclude_count;

    /* ---- INSTALLER 模式专用 ---- */
    int          installer_silent;  /* 非 0 追加静默参数（默认 NSIS "/S"） */
    /* 传给安装器的自定义命令行；非 NULL 时【完全取代】默认的静默参数推断。
     * 例：Inno Setup 静默用 "/VERYSILENT /SUPPRESSMSGBOXES"。 */
    const char*  installer_args;

    /* ---- 回调 ---- */
    au_progress_cb on_progress;
    au_log_cb      on_log;
    void*          userdata;        /* 原样透传给两个回调 */
} au_config_t;

/* ============================================================= 上下文 */
typedef struct au_ctx au_ctx_t;    /* 不透明 */

/* 复制一份配置，创建上下文。失败返回 NULL。cfg 及其字符串在返回后可释放
 * （内部已深拷贝必要字段）。 */
au_ctx_t* au_create(const au_config_t* cfg);
void      au_destroy(au_ctx_t* ctx);

/* 线程安全：可从其他线程调用（如 Ctrl+C 处理函数、UI 取消按钮）。
 * 设置取消标志，进行中的 au_download 会在下一轮循环返回 AU_ERR_CANCELED。 */
void      au_cancel(au_ctx_t* ctx);

/* ============================================================= 检测 */

/* 拉取当前 channel 内【版本号最高】的 release（注意不是列表首项——GitHub 按
 * created_at 排序，取首项会导致降级）。成功时 *out 由调用方用 au_release_free 释放。 */
au_err_t au_fetch_latest(au_ctx_t* ctx, au_release_t** out);

/* 比较 rel 与 cfg.current_version：<0 候选更旧，0 相同，>0 候选更新。
 * 库只报告方向，是否升级/降级由调用方决定。 */
int au_compare_current(au_ctx_t* ctx, const au_release_t* rel);

/* 在 rel->assets 中按 glob 匹配。无匹配或多重匹配均返回 NULL（后者说明
 * pattern 不够精确，宁可报错也不猜）。 */
const au_asset_t* au_pick_asset(const au_release_t* rel, const char* pattern);

/* ============================================================= 下载 / 应用 */

/* 阻塞下载 asset 到 exe 所在目录（保证与自身同卷，供原子 rename）。
 * 落地完整路径写入 out_path（UTF-8）。期间反复触发 on_progress，并在有 digest
 * 时校验 SHA-256。成功返回 AU_OK。 */
au_err_t au_download(au_ctx_t* ctx, const au_asset_t* a, char* out_path, size_t cap);

/* 按 cfg.mode 执行替换并重启。成功时【本函数不返回】——进程已被 ExitProcess 终止。
 * 仅在失败时返回（此时安装保持原状或已回滚）。
 * pkg_path 为 au_download 的产物路径。 */
au_err_t au_apply(au_ctx_t* ctx, const char* pkg_path);

/* ============================================================= 进程握手 */

/* 【新版启动时尽早调用，在任何单实例互斥量创建之前】。
 * au_apply 启动新进程时会追加 --au-wait-pid=<旧PID> 参数；本函数解析它并阻塞
 * 等待旧进程真正退出（最长 timeout_ms，默认 10000）。这样单实例程序不会出现
 * 新旧两个实例并存的窗口期而误判"已在运行"。
 * 非更新启动（无该参数）时是空操作，立即返回。argc/argv 为 main 的原始参数。 */
void au_wait_for_predecessor(int argc, char** argv, int timeout_ms);
/* 宽字符入口，供 WinMain/wWinMain 的 GetCommandLineW 场景。 */
void au_wait_for_predecessor_w(int argc, wchar_t** wargv, int timeout_ms);

/* 【新版启动时尽早调用】清理上一次更新残留的 .old 文件与 _au_tmp 临时目录。
 * 幂等，无残留时是空操作，无需判断本次是否刚更新过。 */
void au_cleanup_after_restart(void);

#ifdef __cplusplus
}
#endif
#endif /* WIN_AUTOUPDATE_H */
