/*
 * platform.h —— Win32 薄封装：日志、编码转换、路径工具。
 * 这一层不依赖任何上层模块，供全项目复用。
 */
#ifndef HUA_PLATFORM_H
#define HUA_PLATFORM_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    HUA_LOG_INFO = 0,
    HUA_LOG_WARN,
    HUA_LOG_ERROR,
    HUA_LOG_OFF,
} HuaLogLevel;

/*
 * 设置日志策略。可在 hua_log_init 前调用（决定是否创建日志），也可在热加载配置时
 * 调用。max_size_mb / retention_days 会在配置层夹到正数范围。
 */
void hua_log_configure(HuaLogLevel min_level, int max_size_mb, int retention_days);

/* 打开/关闭日志文件（exe 同目录下 hua.log，UTF-8 追加写）。 */
void hua_log_init(void);
void hua_log_close(void);

/* 线程安全的格式化日志。宏见下。 */
void hua_logf(HuaLogLevel level, const char *fmt, ...);

#define HUA_LOG_I(...) hua_logf(HUA_LOG_INFO,  __VA_ARGS__)
#define HUA_LOG_W(...) hua_logf(HUA_LOG_WARN,  __VA_ARGS__)
#define HUA_LOG_E(...) hua_logf(HUA_LOG_ERROR, __VA_ARGS__)

/*
 * 编码转换。返回堆分配的字符串，调用方用 hua_free 释放；失败返回 NULL。
 * 内部统一 UTF-16(wchar_t)，与文件/外部边界用 UTF-8。
 */
wchar_t *hua_utf8_to_utf16(const char *s);
char    *hua_utf16_to_utf8(const wchar_t *s);
void     hua_free(void *p);

/* 取当前 exe 所在目录（含末尾反斜杠）写入 out。成功返回 true。 */
bool hua_exe_dir(wchar_t *out, size_t cap);

/* 取 %APPDATA%\hua\ 目录（含末尾反斜杠）写入 out，并确保该目录已创建。
 * 成功返回 true。 */
bool hua_appdata_dir(wchar_t *out, size_t cap);

/* 读取整个文件为堆分配的 NUL 结尾字节缓冲（原样 UTF-8 字节）。
 * 调用方用 hua_free 释放；失败（文件不存在等）返回 NULL。
 *
 * out_len 非 NULL 时写入实际读取的字节数。**要改写文件内容的调用方必须用它**：
 * 仅凭 NUL 结尾无法区分「文本到此结束」与「文件含嵌入 NUL」（如被存成 UTF-16 的
 * ini，其每个 ASCII 字符后都跟一个 \0）——后者用 strlen 遍历会在第一个字符处就
 * 截断，若据此回写就会把用户的文件truncate 掉。 */
char *hua_read_file(const wchar_t *path, size_t *out_len);

/* 判断文件是否存在。 */
bool hua_file_exists(const wchar_t *path);

/* 把字节原样写入文件（覆盖）。成功返回 true。 */
bool hua_write_file(const wchar_t *path, const void *data, size_t len);

#endif /* HUA_PLATFORM_H */
