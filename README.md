# hua（划）· Windows 鼠标手势工具

> **hua（划）** —— "划一下"即手势本身。纯 C17、无 GUI（仅托盘）、`.ini` 配置的 Windows 鼠标手势工具。

按住触发键（默认右键）拖动画出方向 → 识别 → 执行动作（发快捷键 / 运行程序 / 内置命令）。带实时轨迹浮层与动作名提示。

## 特性

- 全局鼠标手势：8 方向九宫格数字串识别（如 `26` = 下·右）。
- 动作类型：`key:` 快捷键、`run:` 运行程序/打开文件、`cmd:` 内置命令。
- **Per-app 覆盖**：同一手势在不同程序执行不同动作（如"下右"全局关窗口、浏览器关标签页）。
- 黑/白名单、**全屏程序中自动禁用**。
- 轨迹浮层：GDI+ 抗锯齿轨迹线 + 方向箭头 + 动作名 OSD + 淡出，颜色/线宽/字号可配。
- 右键触发的**原生菜单还原**（吞按下、松开补发）。
- `.ini` 配置 **UTF-8 + 热加载**（改文件即生效）。
- 默认**管理员权限**运行（手势可作用于提权窗口）；**开机自启**走任务计划程序（静默提权）。
- 仅托盘图标，无配置窗口。
- **内置自动更新**：托盘「检查更新」一键升级（GitHub Release 单文件模式，SHA-256 校验 + 原子替换重启）。

## 下载

[![最新版本](https://img.shields.io/github/v/release/Hunlongyu/hua?sort=semver&label=%E6%9C%80%E6%96%B0%E7%89%88%E6%9C%AC)](https://github.com/Hunlongyu/hua/releases/latest)

前往 [Releases](https://github.com/Hunlongyu/hua/releases/latest) 下载，提供三架构的单文件程序（下载即用，需管理员权限）与含默认配置的 zip：

| 架构 | 适用系统 |
|:---:|:---|
| **x64** | 64 位 Windows 10/11（推荐） |
| **x86** | 32 位 Windows |
| **ARM64** | ARM 设备（Surface Pro X 等），原生运行 |

更新历史见 [CHANGELOG.md](CHANGELOG.md)。发布由推送 `v*` 标签的 GitHub Actions 自动完成（标签版本须与 [`src/version.h`](src/version.h) 一致）。

## 构建

需要 CMake ≥ 3.20 + C17 编译器（MSVC 或 MinGW-w64）。

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

产物：`hua.exe`（MSVC 在 `build/Release/`，MinGW 在 `build/`）。运行需管理员权限；发布时把 `hua.exe` 与 `config/hua.ini` 放同一目录。

第三方仅以源码内联编译：`third_party/ini.c`（inih，配置解析）、`third_party/utest.h`（单元测试）。无运行时 DLL 依赖（除系统自带的 GDI+）。

## 使用

1. 以管理员运行 `hua.exe`，托盘出现图标。
2. 配置文件位置（优先级从高到低）：
   - **exe 同目录 `hua.ini`**（便携模式，最高优先级）；
   - **`%APPDATA%\hua\hua.ini`**（兜底）；两处都没有时，程序会在此**自动创建一份默认配置**。
   日志同理：优先写 exe 同目录 `hua.log`，不可写（如装在 Program Files）则写 `%APPDATA%\hua\hua.log`。
3. 按住右键拖动画手势，松开执行；识别到时浮层底部提示动作名（如"关闭窗口"）。右键快速点击仍弹出系统菜单。
4. 托盘右键菜单：**重载配置 / 打开配置文件 / 开机自启 / 项目地址 / 退出**。版本号见托盘图标悬浮提示。

## 配置（`hua.ini`，UTF-8）

方向数字：`8`上 `2`下 `4`左 `6`右 / `7`左上 `9`右上 `1`左下 `3`右下。

`[General]` 常用项：

| 键 | 说明 |
|---|---|
| `Trigger` | 触发键 `right`/`middle`/`x1`/`x2` |
| `TriggerDistance` | 按下后移动多远（px）才开始手势 |
| `MinDistance` | 方向分段阈值（识别灵敏度，越小越灵敏） |
| `StepDistance` | 采点最小间隔（px，去抖） |
| `Tolerance` | 方向串匹配最大编辑距离（`0`=精确，推荐；上限 `4`） |
| `PauseTimeout` | 鼠标停顿超过此毫秒数则取消手势 |
| `FilterMode` | `blacklist` / `whitelist` |
| `DisableOnFullscreen` | 全屏程序中禁用手势 |
| `RestoreEvent` | 未形成可见轨迹的识别失败是否补发原生按键 |
| `AutoStart` | 开机自启 |
| `LogEnabled` | 是否写日志，默认 `true`；`false` 时完全关闭 |
| `LogLevel` | 最低日志等级：`off` / `error` / `warn` / `info`，默认 `warn` |
| `LogMaxSizeMB` | `hua.log` 达到该体积后轮转为带时间戳的历史文件，默认 `10` MB |
| `LogRetentionDays` | 自动删除超过该天数的历史轮转日志，默认 `2` 天 |
| `ShowTrail`/`ShowActionName`/`TrailArrow`/`RandomColor` | 浮层开关 |
| `TrailColor`/`FailColor` | 命中 / 未命中 OSD 文字描边色 `RRGGBB` |
| `TrailWidth`/`TextSize`/`TextPosition` | 线宽 / 字号 / 文字距底 |
| `TrailMaxLength` | 轨迹绘制长度上限 px（`0`=不限）；超出的旧段丢弃，**不影响识别** |
| `TextFillColor` | OSD 文字镂空填充色 `RRGGBB`（默认白） |
| `TextOutlineWidth`/`TextLetterSpacing` | 文字描边宽度 / 字间距（px） |

手势与动作：

```ini
[Gestures]
6  = key:alt+right        ; → 前进
26 = cmd:close_window     ; ↓→ 关闭窗口

[App:chrome.exe]
26 = key:ctrl+w           ; 浏览器里只关标签页

[App:game.exe]
Enabled = false           ; 该程序禁用手势
```

动作前缀：`key:ctrl+shift+t`（快捷键）、`run:C:\tools\a.exe`（运行/打开）、`cmd:close_window`（内置命令）。

**`cmd:none` = 显式无动作**，用于在 per-app 中屏蔽某个全局手势（不会回落到全局映射），浮层照常提示「手势无动作」：

```ini
[Gestures]
28 = cmd:scroll_bottom    ; 近似沿同一竖线下、上，可绑定独立动作
39 = key:f5               ; 明显向右展开的 V 形刷新

[App:powerpnt.exe]
39 = cmd:none             ; PPT 中屏蔽 V 形刷新（F5 是放映）
```

内置命令：`close_window` `minimize` `maximize` `restore` `toggle_maximize` `scroll_top` `scroll_bottom` `volume_up` `volume_down` `volume_mute` `media_play` `copy` `paste` `open_exe_dir`（打开当前程序所在目录）。

改完在托盘点"重载配置"，或直接保存文件即自动热加载。

**容量上限**：全局手势 128 条、`[App:]` 节 64 个、每个 `[App:]` 内手势 64 条；方向串最长 15 字符。超出的条目会被丢弃并在 `hua.log` 里告警。

**排查**：配置没生效时先看 `hua.log`——拼错的键名、无法识别的开关值（开关只接受 `true`/`false`，也认 `1`/`0`、`yes`/`no`、`on`/`off`）、超出容量的条目都会记录在那里。

## 许可证

本项目以 [MIT License](LICENSE) 发布。

第三方组件（源码内联编译，无运行时 DLL 依赖）：

| 组件 | 用途 | 许可证 |
|---|---|---|
| [inih](https://github.com/benhoyt/inih) | `.ini` 配置解析 | BSD-3-Clause，见 [third_party/inih-LICENSE.txt](third_party/inih-LICENSE.txt) |
| [utest.h](https://github.com/sheredom/utest.h) | 单元测试框架（仅测试，不进产物） | Unlicense |

## 设计文档

见 [docs/hua-design.md](docs/hua-design.md)。
