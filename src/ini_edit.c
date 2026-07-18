/*
 * ini_edit.c —— 见 ini_edit.h。
 */
#include "ini_edit.h"

#include <stdio.h>
#include <string.h>

/* ASCII 大小写不敏感的前缀比较：s 是否以 prefix（全 ASCII）起头。
 * 自带实现而不依赖 _strnicmp/strncasecmp，保持本模块可在任意平台单测。 */
static bool ci_starts_with(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

/* s 是已跳过前导空白的行首。判定是否为 [General] 节头。 */
static bool line_is_general(const char *s)
{
    return ci_starts_with(s, "[General]");
}

/* 判定是否为 AutoStart 键行。整键匹配：只比前缀会把 AutoStartFoo=1 也改写掉。 */
static bool line_is_autostart(const char *s)
{
    if (!ci_starts_with(s, "AutoStart"))
        return false;
    const char *p = s + 9;   /* 跳过 "AutoStart" */
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == '=';
}

/* 向 out 追加 n 字节；越界（含须为结尾 '\0' 预留的 1 字节）时返回 false，不写出。 */
static bool emit(char *out, size_t cap, size_t *pos, const char *s, size_t n)
{
    if (*pos >= cap || n >= cap - *pos)
        return false;
    memcpy(out + *pos, s, n);
    *pos += n;
    return true;
}

size_t ini_set_autostart(const char *in, size_t in_len, bool enable,
                         char *out, size_t out_cap)
{
    if (!in || !out || out_cap == 0)
        return HUA_INI_EDIT_FAIL;
    /*
     * 含嵌入 NUL（典型是被记事本「另存为 Unicode」存成了 UTF-16）：下面按 strchr/
     * strlen 逐行遍历会在第一个 NUL 处就停下，于是「成功」产出一份只剩几字节的内容
     * ——调用方据此替换原文件，用户整份手势表静默永久丢失。宁可拒绝。
     */
    if (in_len != strlen(in))
        return HUA_INI_EDIT_FAIL;

    /* 用 snprintf 复用与旧实现完全一致的格式串，保证输出逐字节相同。 */
    char autoline[40];
    int an = snprintf(autoline, sizeof autoline, "AutoStart       = %s\r\n",
                      enable ? "true" : "false");
    if (an < 0 || (size_t)an >= sizeof autoline)
        return HUA_INI_EDIT_FAIL;   /* 不可能发生，纯防御 */

    /*
     * 先扫一遍：[General] 里到底有没有 AutoStart 行。
     * 必须带「节」的概念——AutoStart 只属于 [General]。若把它追加到文件末尾，
     * 那里通常是最后一个 [App:xxx] 节（内置默认配置就以 [App:msedge.exe] 结尾），
     * 于是这行会被解析成该程序的一条垃圾手势，开关永不生效。
     */
    bool have_autostart = false;
    {
        bool in_gen = false;
        const char *l = in;
        while (*l) {
            const char *e = strchr(l, '\n');
            const char *s = l;
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s == '[')
                in_gen = line_is_general(s);
            else if (in_gen && line_is_autostart(s)) {
                have_autostart = true;
                break;
            }
            if (!e)
                break;
            l = e + 1;
        }
    }

    size_t pos = 0;
    bool in_gen = false;
    bool inserted = false;
    const char *line = in;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);
        const char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        bool is_hdr = (*s == '[');
        if (is_hdr)
            in_gen = line_is_general(s);

        if (in_gen && !is_hdr && have_autostart && line_is_autostart(s)) {
            if (!emit(out, out_cap, &pos, autoline, (size_t)an))   /* 就地替换 */
                return HUA_INI_EDIT_FAIL;
        } else {
            if (line_len > 0 && !emit(out, out_cap, &pos, line, line_len))   /* 含行尾 \r */
                return HUA_INI_EDIT_FAIL;
            if (eol && !emit(out, out_cap, &pos, "\n", 1))
                return HUA_INI_EDIT_FAIL;
            /* 原文件没有该键：紧跟在 [General] 头之后插入，而不是丢到文件末尾。 */
            if (is_hdr && in_gen && !have_autostart && !inserted) {
                if (!eol && !emit(out, out_cap, &pos, "\n", 1))   /* 头恰是最后一行且无换行 */
                    return HUA_INI_EDIT_FAIL;
                if (!emit(out, out_cap, &pos, autoline, (size_t)an))
                    return HUA_INI_EDIT_FAIL;
                inserted = true;
            }
        }
        if (!eol)
            break;
        line = eol + 1;
    }
    /* 整份 ini 连 [General] 节都没有：补一个完整的节，而非裸键追加到末尾。 */
    if (!have_autostart && !inserted) {
        char genblock[64];
        int gn = snprintf(genblock, sizeof genblock,
                          "\r\n[General]\r\nAutoStart       = %s\r\n",
                          enable ? "true" : "false");
        if (gn < 0 || (size_t)gn >= sizeof genblock)
            return HUA_INI_EDIT_FAIL;
        if (!emit(out, out_cap, &pos, genblock, (size_t)gn))
            return HUA_INI_EDIT_FAIL;
    }

    out[pos] = '\0';
    return pos;
}
