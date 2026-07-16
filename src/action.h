/*
 * action.h —— 动作执行：key: / run: / cmd:。
 *
 * 值前缀约定：
 *   key:ctrl+shift+t   发送快捷键组合
 *   run:C:\tools\a.exe 运行程序 / 打开文件 / URL
 *   cmd:close_window   内置命令
 *
 * 快捷键解析（action_parse_key）是纯逻辑，可单测；实际 SendInput/ShellExecute
 * 依赖 Win32，走手动冒烟。
 */
#ifndef HUA_ACTION_H
#define HUA_ACTION_H

#include <windows.h>
#include <stdbool.h>

typedef struct {
    bool ctrl, alt, shift, win;
    WORD vk;   /* 主键虚拟键码；0 表示无有效主键 */
} KeyCombo;

/*
 * 解析 "ctrl+shift+t" / "alt+left" / "f5" / "esc" 形式的快捷键描述。
 * 大小写不敏感。成功（含有效主键）返回 true，否则 false。
 */
bool action_parse_key(const char *spec, KeyCombo *out);

/*
 * 执行一条动作值。target 是手势起点下方锁定的目标窗口；窗口命令直接
 * 操作它，键盘类动作会先激活它再通过 SendInput 发送。
 * 成功返回 true。
 */
bool action_execute(const char *value, HWND target);

/*
 * 返回动作的友好显示名（供浮层 OSD 提示用户），如 "关闭窗口" / "前进"。
 * 返回静态字符串或指向 value 内部，勿释放。value 为 NULL 返回空串。
 */
const char *action_label(const char *value);

#endif /* HUA_ACTION_H */
