/*
 * au_internal.h — 库内部共享声明。不对外暴露，不随库分发给消费者。
 */
#ifndef AU_INTERNAL_H
#define AU_INTERNAL_H

#include "win_autoupdate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---------------- 上下文 ---------------- */
struct au_ctx {
    /* 配置的深拷贝（字符串独立持有，调用方可在 au_create 后释放原配置）。 */
    char*        owner;
    char*        repo;
    au_version_t current;
    char*        asset_pattern;
    au_mode_t    mode;
    au_channel_t channel;
    char*        user_agent;
    char*        token;

    wchar_t**    mirrors;       /* 宽字符模板，含 {url} */
    size_t       mirror_count;

    int          timeout_ms;
    int          retry_max;

    wchar_t**    zip_exclude;   /* basename，宽字符，小写化便于比较 */
    size_t       zip_exclude_count;

    int          installer_silent;
    char*        installer_args;

    au_progress_cb on_progress;
    au_log_cb      on_log;
    void*          userdata;

    volatile LONG  cancel_flag; /* Interlocked 访问 */
};

/* ---------------- au_str.c：编码与 glob ---------------- */

/* UTF-8 → 新分配的宽字符串（调用方 free）。失败返回 NULL。 */
wchar_t* au_utf8_to_wide(const char* s);
/* 宽字符 → 新分配的 UTF-8 串（调用方 free）。失败返回 NULL。 */
char*    au_wide_to_utf8(const wchar_t* w);
/* glob：仅支持 * 与 ?，大小写不敏感。pat/str 均为 UTF-8。匹配返回 1。 */
int      au_glob_match(const char* pat, const char* str);
/* strdup 的可移植版（NULL 入参返回 NULL）。 */
char*    au_strdup(const char* s);

/* ---------------- au_hash.c ---------------- */

/* 计算 path（宽字符）文件的 SHA-256，写入 out_hex（至少 65 字节，小写十六进制）。 */
au_err_t au_sha256_file(const wchar_t* path, char out_hex[65]);

/* ---------------- au_http.c ---------------- */

/* HTTP 响应元信息。 */
typedef struct {
    int      status;            /* HTTP 状态码 */
    int      ratelimit_remaining; /* X-RateLimit-Remaining，缺失为 -1 */
} au_http_meta;

/* GET url 到内存。*out 为新分配的 NUL 结尾缓冲（调用方 free），*len 为字节数。
 * 单个 URL，不做镜像轮换（镜像逻辑在调用方按需套用）。 */
au_err_t au_http_get_mem(au_ctx_t* ctx, const wchar_t* url,
                         char** out, size_t* len, au_http_meta* meta);

/* GET url 流式写入 fp（已打开的二进制写文件）。触发进度回调与取消检查。 */
au_err_t au_http_get_file(au_ctx_t* ctx, const wchar_t* url, HANDLE hFile,
                          au_http_meta* meta);

/* 用镜像模板生成候选 URL 列表：把每个模板里的 {url} 替换为 raw_url。
 * 返回新分配的宽字符指针数组与个数，调用方逐个 free 再 free 数组。 */
wchar_t** au_build_mirror_urls(au_ctx_t* ctx, const char* raw_url, size_t* count);

/* ---------------- au_fs.c ---------------- */

/* 取当前 exe 的完整路径（宽字符，新分配，调用方 free）。 */
wchar_t* au_self_path(void);
/* 取 path 所在目录（新分配）。 */
wchar_t* au_dir_of(const wchar_t* path);
/* 探测目录可写：成功建删一个临时文件返回 1。 */
int      au_dir_writable(const wchar_t* dir);
/* 用系统 tar.exe 解压 zip 到 dest_dir。成功返回 AU_OK。 */
au_err_t au_extract_zip(au_ctx_t* ctx, const wchar_t* zip, const wchar_t* dest_dir);
/* 递归复制 src_dir 下全部内容到 dst_dir，命中 zip_exclude 且目标已存在则跳过。 */
au_err_t au_copy_tree(au_ctx_t* ctx, const wchar_t* src_dir, const wchar_t* dst_dir);
/* 递归删除目录。 */
void     au_rmtree(const wchar_t* dir);
/* 判断文件是否存在。 */
int      au_file_exists(const wchar_t* path);

/* ---------------- au_gh.c：可离线测试的选择逻辑 ---------------- */

/* 从一段 /releases 的 JSON 数组文本中，按通道筛选并取版本号最大的 release。
 * 与网络无关，供单测直接喂 fixture。成功时 *out 由调用方 au_release_free 释放。 */
au_err_t au_select_best_from_json(au_ctx_t* ctx, const char* json, size_t len,
                                  au_release_t** out);

/* ---------------- 日志辅助 ---------------- */
void au_logf(au_ctx_t* ctx, au_log_level_t lv, const char* fmt, ...);

/* 取消检查。 */
static inline int au_is_canceled(au_ctx_t* ctx) {
    return ctx && InterlockedCompareExchange(&ctx->cancel_flag, 0, 0) != 0;
}

#endif /* AU_INTERNAL_H */
