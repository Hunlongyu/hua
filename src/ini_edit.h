/*
 * ini_edit.h —— ini 文本的 AutoStart 改写，纯文本变换、零 Win32/文件 IO 依赖，
 * 便于单测。此前这段逻辑埋在 main.c 的 FILE* 循环里、只能靠运行整个程序来验证，
 * 而它踩过至少四个坑（嵌入 NUL 毁配置、临时路径截断别名到原文件、AutoStart 前缀
 * 误匹配、无节感知追加把裸键塞进 [App:] 节）——正是该被表驱动测试钉住的东西。
 */
#ifndef HUA_INI_EDIT_H
#define HUA_INI_EDIT_H

#include <stddef.h>
#include <stdbool.h>

/* ini_set_autostart 的失败返回值（与合法的 0 长度输出区分开）。 */
#define HUA_INI_EDIT_FAIL ((size_t)-1)

/*
 * 把 ini 文本中 [General] 节的 AutoStart 值改写为 enable，其余内容尽量原样保留：
 *   - [General] 内已有 AutoStart 行 → 就地替换整行（沿用既有行为，行尾注释不保留）；
 *   - 有 [General] 但无该行 → 紧跟节头之后插入；
 *   - 连 [General] 都没有 → 在文本末尾补一个完整的 [General] 节。
 * AutoStart 只认 [General] 节内的整键；[App:] 节里的同名行、以及 AutoStartFoo 这类
 * 前缀相同的键都不受影响。
 *
 * in/in_len：输入文本与其字节数。in_len != strlen(in)（含嵌入 NUL，典型是被存成
 *   UTF-16）时拒绝处理并返回 HUA_INI_EDIT_FAIL——否则会产出被首个 NUL 截断的内容，
 *   调用方据此替换原文件就会静默丢掉用户整份配置。
 * out/out_cap：输出缓冲，写入以 '\0' 结尾的结果。out_cap 不足时返回失败而非截断；
 *   建议 out_cap >= in_len * 2 + 64。
 * 返回结果字节数（不含结尾 '\0'）；失败（嵌入 NUL / 缓冲不足）返回 HUA_INI_EDIT_FAIL。
 */
size_t ini_set_autostart(const char *in, size_t in_len, bool enable,
                         char *out, size_t out_cap);

#endif /* HUA_INI_EDIT_H */
