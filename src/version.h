/* 版本号单一来源（供 hua.h 与 hua.rc 共用；仅 #define，windres/rc 可安全解析）。
 * 改版本只改这里的三个数字：C 代码的 HUA_VERSION 与 exe 的 VERSIONINFO 都由此推导。 */
#ifndef HUA_VERSION_H
#define HUA_VERSION_H

#define HUA_VER_MAJOR 1
#define HUA_VER_MINOR 0
#define HUA_VER_PATCH 0

#define HUA_STRINGIZE_(x) #x
#define HUA_STRINGIZE(x)  HUA_STRINGIZE_(x)

/* "1.0.0" */
#define HUA_VERSION_STR  HUA_STRINGIZE(HUA_VER_MAJOR) "." \
                         HUA_STRINGIZE(HUA_VER_MINOR) "." \
                         HUA_STRINGIZE(HUA_VER_PATCH)

#endif /* HUA_VERSION_H */
