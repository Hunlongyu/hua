/*
 * default_ini.h —— 内置默认配置文本（UTF-8）。
 * 当 exe 同级与 %APPDATA%\hua 都没有 hua.ini 时，写一份到 AppData 兜底。
 * 内容需与 config/hua.ini 保持一致。
 */
#ifndef HUA_DEFAULT_INI_H
#define HUA_DEFAULT_INI_H

static const char HUA_DEFAULT_INI[] =
"; ===================== hua 配置 =====================\n"
"; 保存为 UTF-8。方向数字：8上 2下 4左 6右 / 7左上 9右上 1左下 3右下\n"
"; 改完在托盘点「重载配置」即生效（保存即自动热加载）。\n"
"\n"
"[General]\n"
"; --- 触发与识别 ---\n"
"Trigger         = right      ; right | middle | x1 | x2\n"
"TriggerDistance = 5          ; 按下后移动多远（px）才开始手势\n"
"MinDistance     = 20         ; 方向分段阈值（识别灵敏度，越小越灵敏）\n"
"StepDistance    = 12         ; 采点最小间隔像素（去抖）\n"
"Tolerance       = 0          ; 方向串匹配最大编辑距离（0=精确，推荐）\n"
"PauseTimeout    = 1000       ; 鼠标停顿超过此毫秒数则取消手势\n"
"FilterMode      = blacklist  ; blacklist | whitelist\n"
"DisableOnFullscreen = true   ; 全屏程序中禁用手势\n"
"AutoStart       = false      ; 开机自启（任务计划程序 + 最高权限）\n"
"RestoreEvent    = true       ; 未形成轨迹的识别失败是否补发原生按键\n"
"\n"
"; --- 浮层外观 ---\n"
"ShowTrail       = true       ; 绘制过程显示轨迹线\n"
"ShowActionName  = true       ; 手势结束显示动作名\n"
"TrailArrow      = true       ; 轨迹末端画方向箭头\n"
"RandomColor     = false      ; 轨迹随机颜色\n"
"TrailColor      = 00A0FF     ; 手势颜色 / 命中动作名描边色 RRGGBB\n"
"FailColor       = 666666     ; 未命中「手势无动作」描边色 RRGGBB\n"
"TrailWidth      = 3          ; 轨迹线宽\n"
"TrailMaxLength  = 2500       ; 轨迹绘制长度上限（px，0=不限）；超出部分旧段丢弃，不影响识别\n"
"TextSize        = 26         ; 动作名字号\n"
"TextPosition    = 150        ; 动作名距屏幕底部高度（px）\n"
"TextFillColor   = FFFFFF     ; 文字镂空填充色 RRGGBB（默认白）\n"
"TextOutlineWidth= 3          ; 文字描边宽度（px，0=不描边）\n"
"TextLetterSpacing = 4        ; 文字字间距（px）\n"
"\n"
"; ---------- 全局默认手势：方向串 = 动作（按九宫格数字升序） ----------\n"
"[Gestures]\n"
"1  = cmd:minimize         ; ↙ 左下    最小化\n"
"2  = cmd:scroll_bottom    ; ↓ 下      滚动到底部\n"
"3  = key:delete           ; ↘ 右下    删除\n"
"4  = key:alt+left         ; ← 左      后退\n"
"6  = key:alt+right        ; → 右      前进\n"
"7  = key:esc              ; ↖ 左上    Esc\n"
"8  = cmd:scroll_top       ; ↑ 上      滚动到顶部\n"
"9  = cmd:toggle_maximize  ; ↗ 右上    最大化 / 还原（切换）\n"
"26 = cmd:close_window     ; ↓→ 下右   关闭窗口\n"
"39 = key:f5               ; ↘↗ V 形   刷新（浏览器/资源管理器等；F5 在 PPT/IDE 中另有含义）\n"
"86 = cmd:open_exe_dir     ; ↑→ 上右   打开当前程序所在目录\n"
"\n"
"; ---------- 程序专属覆盖 ----------\n"
"; 动作用 cmd:none 表示「此程序内显式屏蔽该手势」（不会回落到上面的全局映射）。\n"
"[App:chrome.exe]\n"
"26 = key:ctrl+w              ; 下·右：浏览器里只关当前标签页\n"
"\n"
"[App:msedge.exe]\n"
"26 = key:ctrl+w\n";

#endif /* HUA_DEFAULT_INI_H */
