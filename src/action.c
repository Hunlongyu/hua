/*
 * action.c —— 见 action.h。
 */
#include "action.h"
#include "platform.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---------------- 快捷键解析（纯逻辑） ---------------- */

static void str_tolower_inplace(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

/* 键名 → 虚拟键码；未知返回 0。传入的 token 已小写去空格。 */
static WORD key_name_to_vk(const char *t)
{
    size_t len = strlen(t);
    if (len == 0)
        return 0;

    if (len == 1) {
        char c = t[0];
        if (c >= 'a' && c <= 'z') return (WORD)(c - 'a' + 'A');
        if (c >= '0' && c <= '9') return (WORD)c;
        return 0;
    }

    /* 功能键 f1..f24 */
    if (t[0] == 'f' && isdigit((unsigned char)t[1])) {
        int n = atoi(t + 1);
        if (n >= 1 && n <= 24)
            return (WORD)(VK_F1 + (n - 1));
    }

    static const struct { const char *name; WORD vk; } map[] = {
        {"esc", VK_ESCAPE},   {"escape", VK_ESCAPE},
        {"enter", VK_RETURN}, {"return", VK_RETURN},
        {"tab", VK_TAB},      {"space", VK_SPACE},
        {"backspace", VK_BACK}, {"back", VK_BACK},
        {"delete", VK_DELETE}, {"del", VK_DELETE},
        {"insert", VK_INSERT}, {"ins", VK_INSERT},
        {"home", VK_HOME},    {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
        {"pagedown", VK_NEXT},{"pgdn", VK_NEXT},
        {"left", VK_LEFT},    {"right", VK_RIGHT},
        {"up", VK_UP},        {"down", VK_DOWN},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(t, map[i].name) == 0)
            return map[i].vk;

    return 0;
}

bool action_parse_key(const char *spec, KeyCombo *out)
{
    if (out) {
        out->ctrl = out->alt = out->shift = out->win = false;
        out->vk = 0;
    }
    if (!spec || !out)
        return false;

    char buf[128];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    str_tolower_inplace(buf);

    bool has_main = false;
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        /* 去首尾空格 */
        while (*tok == ' ') tok++;
        size_t l = strlen(tok);
        while (l > 0 && tok[l - 1] == ' ') tok[--l] = '\0';

        if (strcmp(tok, "ctrl") == 0 || strcmp(tok, "control") == 0) {
            out->ctrl = true;
        } else if (strcmp(tok, "alt") == 0) {
            out->alt = true;
        } else if (strcmp(tok, "shift") == 0) {
            out->shift = true;
        } else if (strcmp(tok, "win") == 0 || strcmp(tok, "super") == 0 ||
                   strcmp(tok, "meta") == 0) {
            out->win = true;
        } else {
            WORD vk = key_name_to_vk(tok);
            if (vk == 0 || has_main)   /* 未知键 或 出现第二个主键 */
                return false;
            out->vk = vk;
            has_main = true;
        }
    }
    return out->vk != 0;
}

/* ---------------- 键盘合成 ---------------- */

static void push_key(INPUT *arr, int *n, WORD vk, bool up)
{
    INPUT e;
    memset(&e, 0, sizeof(e));
    e.type = INPUT_KEYBOARD;
    e.ki.wVk = vk;
    e.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    arr[(*n)++] = e;
}

static bool send_key(const KeyCombo *k)
{
    INPUT in[10];
    int n = 0;
    if (k->ctrl)  push_key(in, &n, VK_CONTROL, false);
    if (k->alt)   push_key(in, &n, VK_MENU,    false);
    if (k->shift) push_key(in, &n, VK_SHIFT,   false);
    if (k->win)   push_key(in, &n, VK_LWIN,    false);
    push_key(in, &n, k->vk, false);
    push_key(in, &n, k->vk, true);
    if (k->win)   push_key(in, &n, VK_LWIN,    true);
    if (k->shift) push_key(in, &n, VK_SHIFT,   true);
    if (k->alt)   push_key(in, &n, VK_MENU,    true);
    if (k->ctrl)  push_key(in, &n, VK_CONTROL, true);

    UINT sent = SendInput((UINT)n, in, sizeof(INPUT));
    return sent == (UINT)n;
}

/*
 * SendInput 只能送到前台窗口。手势可能开始于非活动窗口，因此键盘类动作
 * 执行前要先激活起点下方锁定的目标。仅临时连接当前前台输入队列，不修改
 * 系统的 ForegroundLockTimeout；后者是 StrokesPlus 旧版采用但过于侵入的做法。
 */
static bool activate_target(HWND target)
{
    if (!target || !IsWindow(target))
        return false;

    HWND foreground = GetForegroundWindow();
    if (foreground == target)
        return true;

    DWORD self_tid = GetCurrentThreadId();
    DWORD foreground_tid = foreground
                               ? GetWindowThreadProcessId(foreground, NULL)
                               : 0;
    bool attached = foreground_tid && foreground_tid != self_tid &&
                    AttachThreadInput(self_tid, foreground_tid, TRUE);

    BOOL activated = SetForegroundWindow(target);

    if (attached)
        AttachThreadInput(self_tid, foreground_tid, FALSE);

    return activated || GetForegroundWindow() == target;
}

static bool send_key_to_target(const KeyCombo *k, HWND target)
{
    if (!activate_target(target))
        return false;
    return send_key(k);
}

/* 单键点按（无修饰），用于音量/媒体键。 */
static bool tap(WORD vk)
{
    KeyCombo k = {0};
    k.vk = vk;
    return send_key(&k);
}

/* ---------------- 内置命令表 ---------------- */

static bool exec_cmd(const char *name, HWND target)
{
    if (strcmp(name, "close_window") == 0) {
        if (target) PostMessageW(target, WM_CLOSE, 0, 0);
        return target != NULL;
    }
    if (strcmp(name, "minimize") == 0) {
        if (target) ShowWindow(target, SW_MINIMIZE);
        return target != NULL;
    }
    if (strcmp(name, "maximize") == 0) {
        if (target) ShowWindow(target, SW_MAXIMIZE);
        return target != NULL;
    }
    if (strcmp(name, "restore") == 0) {
        if (target) ShowWindow(target, SW_RESTORE);
        return target != NULL;
    }
    if (strcmp(name, "toggle_maximize") == 0) {
        if (target)
            ShowWindow(target, IsZoomed(target) ? SW_RESTORE : SW_MAXIMIZE);
        return target != NULL;
    }
    if (strcmp(name, "scroll_top") == 0) {
        KeyCombo k = {0}; k.ctrl = true; k.vk = VK_HOME;
        return send_key_to_target(&k, target);
    }
    if (strcmp(name, "scroll_bottom") == 0) {
        KeyCombo k = {0}; k.ctrl = true; k.vk = VK_END;
        return send_key_to_target(&k, target);
    }
    if (strcmp(name, "volume_up") == 0)   return tap(VK_VOLUME_UP);
    if (strcmp(name, "volume_down") == 0) return tap(VK_VOLUME_DOWN);
    if (strcmp(name, "volume_mute") == 0) return tap(VK_VOLUME_MUTE);
    if (strcmp(name, "media_play") == 0)  return tap(VK_MEDIA_PLAY_PAUSE);
    if (strcmp(name, "copy") == 0) {
        KeyCombo k = {0}; k.ctrl = true; k.vk = 'C';
        return send_key_to_target(&k, target);
    }
    if (strcmp(name, "paste") == 0) {
        KeyCombo k = {0}; k.ctrl = true; k.vk = 'V';
        return send_key_to_target(&k, target);
    }
    if (strcmp(name, "open_exe_dir") == 0) {
        /* 打开目标窗口所属进程 exe 所在目录（资源管理器）。 */
        if (!target)
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        if (!pid)
            return false;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h)
            return false;
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(h, 0, path, &n);
        CloseHandle(h);
        if (!ok)
            return false;
        wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash)
            return false;
        *slash = L'\0';   /* 截断文件名，保留目录 */
        HINSTANCE r = ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
        return (INT_PTR)r > 32;
    }
    return false;   /* 未知命令 */
}

/* ---------------- 友好显示名 ---------------- */

const char *action_label(const char *value)
{
    if (!value)
        return "";

    if (strncmp(value, "cmd:", 4) == 0) {
        const char *n = value + 4;
        if (!strcmp(n, "close_window"))    return "关闭窗口";
        if (!strcmp(n, "minimize"))        return "最小化";
        if (!strcmp(n, "maximize"))        return "最大化";
        if (!strcmp(n, "restore"))         return "还原";
        if (!strcmp(n, "toggle_maximize")) return "最大化/还原";
        if (!strcmp(n, "scroll_top"))      return "滚到顶部";
        if (!strcmp(n, "scroll_bottom"))   return "滚到底部";
        if (!strcmp(n, "volume_up"))       return "音量 +";
        if (!strcmp(n, "volume_down"))     return "音量 -";
        if (!strcmp(n, "volume_mute"))     return "静音";
        if (!strcmp(n, "media_play"))      return "播放/暂停";
        if (!strcmp(n, "copy"))            return "复制";
        if (!strcmp(n, "paste"))           return "粘贴";
        if (!strcmp(n, "open_exe_dir"))    return "打开程序目录";
        return n;
    }
    if (strncmp(value, "key:", 4) == 0) {
        const char *k = value + 4;
        if (!_stricmp(k, "alt+left"))  return "后退";
        if (!_stricmp(k, "alt+right")) return "前进";
        if (!_stricmp(k, "f5"))        return "刷新";
        if (!_stricmp(k, "ctrl+w"))    return "关闭标签页";
        if (!_stricmp(k, "ctrl+t"))    return "新建标签页";
        if (!_stricmp(k, "delete"))    return "删除";
        if (!_stricmp(k, "esc"))       return "Esc";
        return k;   /* 其余显示按键组合本身，如 ctrl+shift+t */
    }
    if (strncmp(value, "run:", 4) == 0)
        return "运行程序";
    return value;
}

/* ---------------- 分发 ---------------- */

bool action_execute(const char *value, HWND target)
{
    if (!value)
        return false;

    if (strncmp(value, "key:", 4) == 0) {
        KeyCombo k;
        if (!action_parse_key(value + 4, &k))
            return false;
        return send_key_to_target(&k, target);
    }
    if (strncmp(value, "run:", 4) == 0) {
        wchar_t *w = hua_utf8_to_utf16(value + 4);
        if (!w)
            return false;
        HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        hua_free(w);
        return (INT_PTR)r > 32;   /* ShellExecute 约定：>32 为成功 */
    }
    if (strncmp(value, "cmd:", 4) == 0) {
        return exec_cmd(value + 4, target);
    }
    return false;
}
