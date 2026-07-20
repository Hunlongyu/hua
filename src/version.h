/* 版本号单一来源（供 hua.h 与 hua.rc 共用；仅 #define，windres/rc 可安全解析）。
 * 改版本只改这里的三个数字：C 代码的 HUA_VERSION 与 exe 的 VERSIONINFO 都由此推导。 */
#ifndef HUA_VERSION_H
#define HUA_VERSION_H

/* 每次改动代码都要升 PATCH（用户 2026-07 要求）：启动日志会打出版本号，
 * 用户据此一眼判断跑的是不是刚构建的那个 exe。曾经发生过 exe 被运行中的进程
 * 锁住、链接失败却被误判为构建成功，用户拿旧二进制测了一整轮的事。 */
#define HUA_VER_MAJOR 1
#define HUA_VER_MINOR 0
#define HUA_VER_PATCH 14

#define HUA_STRINGIZE_(x) #x
#define HUA_STRINGIZE(x)  HUA_STRINGIZE_(x)

/* 例如 "1.0.9"。 */
#define HUA_VERSION_STR  HUA_STRINGIZE(HUA_VER_MAJOR) "." \
                         HUA_STRINGIZE(HUA_VER_MINOR) "." \
                         HUA_STRINGIZE(HUA_VER_PATCH)

#endif /* HUA_VERSION_H */
