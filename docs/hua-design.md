# Hua（划）· 鼠标手势工具开发设计文档

> **hua（划）** — "划一下"即手势的动作本身。Windows 平台、纯 C、无 GUI（仅托盘）、`.ini` 配置的鼠标手势工具。
> 目标是复刻 MouseInc / Aitiy 的**手势子集**，去掉截图、翻译、贴图等一切额外功能。

文档版本 v0.5 ｜ 平台 Windows 10/11 ｜ 语言 C17 ｜ 构建 CMake + GitHub Actions

---

## 0. 关键设计决策（本版定稿）

以下三点在此前讨论中未最终确认，本版按推荐默认值定稿，**均可在 `.ini` 或编译期改动**：

| 决策 | 选定方案 | 理由 |
|---|---|---|
| 触发键 | **右键**，`.ini` 可切 `middle/x1/x2` | 最贴近 MouseInc 体验；代价是要实现"右键补发还原" |
| 方向编码 | **8 方向九宫格数字 + 编辑距离容错** | 单字符统一、ini 整齐；容错让画歪也能识别 |
| 浮层显示 | **轨迹线 + 实时动作名 OSD** | 反馈最好，最接近 MouseInc 观感 |

---

## 1. 目标与非目标

### 1.1 目标（In Scope）
- 全局鼠标手势识别，按住触发键拖动 → 识别方向序列 → 执行动作。
- 动作类型：发送快捷键、运行程序/打开文件、内置命令。
- **Per-app 映射**：同一手势在不同前台程序执行不同动作（如"下右"全局=关窗口，浏览器里=关标签页）。
- 黑/白名单：控制手势在哪些程序生效。
- 屏幕浮层：实时轨迹线 + 识别到的方向/动作名提示。
- **全屏程序中默认禁用手势**（可配）：检测到全屏独占/无边框全屏时不触发。
- **默认以管理员权限运行**：使手势可作用于提权窗口（任务管理器、以管理员运行的程序等）。
- **开机自启**（可配）：因默认提权，自启采用任务计划程序 + 最高权限，避免每次开机弹 UAC。
- 配置全部走 UTF-8 编码的 `.ini`，支持热加载（改文件即生效，无需重启）。
- 仅托盘图标，无任何配置窗口。

### 1.2 非目标（Out of Scope）
- 截图、OCR、贴图、翻译、超级拖拽、滚轮增强等 MouseInc 的其余功能。
- 任意形状手势识别（画圆、画字母）——一期只做方向序列；二期可选接 `$1` 模板匹配。
- 跨平台。仅 Windows。
- 多显示器 DPI 完美适配——一期保证主屏正确，高 DPI 做基础处理。

---

## 2. 技术选型与依赖

| 关注点 | 选择 | 说明 |
|---|---|---|
| 输入捕获 | `SetWindowsHookEx(WH_MOUSE_LL)` | 全局低级鼠标钩子；本程序以管理员运行，故可覆盖提权窗口 |
| 权限 | manifest `requireAdministrator` | 默认提权运行；配合 UIAccess 考量见 §4.7 |
| 全屏检测 | `SHQueryUserNotificationState` + 几何比对 | 独占全屏 / 无边框全屏时禁用手势 |
| 开机自启 | 任务计划程序（最高权限） | 提权程序静默自启，避免 UAC 弹窗 |
| 主循环载体 | message-only 窗口 (`HWND_MESSAGE`) | 无可见窗口也能跑消息循环，供钩子与托盘工作 |
| 托盘图标 | `Shell_NotifyIcon` | 右键菜单：重载配置 / 打开 ini / 退出 |
| 浮层绘制 | 分层窗口 + **GDI+** | 抗锯齿轨迹线；`WS_EX_LAYERED|TRANSPARENT|TOPMOST|NOACTIVATE` |
| 配置解析 | **inih**（benhoyt/inih，New BSD，~300 行单文件） | SAX 回调式，契合"边解析边建模型"；UTF-8 字节透明；弃用 ANSI 的 `GetPrivateProfileString`。备选 minIni（Apache-2.0，自带写回） |
| 动作执行 | `SendInput` / `CreateProcessW` / `ShellExecuteW` | |
| 字符集 | 内部统一 `wchar_t`(UTF-16)，文件 UTF-8 | 边界处转码 |
| 单元测试 | `utest.h`（单头文件）+ CTest | 纯函数（识别、编辑距离、ini 解析）零依赖可测 |

**外部依赖：Win32 + GDI+（系统自带）。** 第三方仅以**源码内联**方式编译进来（inih 的 `ini.c/ini.h`、测试用 `utest.h`），无任何运行时 DLL 依赖。

---

## 3. 架构总览

分层，单向依赖（上层依赖下层，下层不知上层）：

```
┌───────────────────────────────────────────────┐
│ app         主程序：消息循环 / 托盘 / 生命周期     │
├───────────────────────────────────────────────┤
│ overlay     浮层渲染：轨迹线 + 动作名 OSD          │
│ action      动作执行：key / run / cmd            │
├───────────────────────────────────────────────┤
│ context     前台进程名获取、per-app 映射解析       │
│ recognizer  采点 → 方向量化 → 序列压缩 → 编辑距离匹配 │
│ config      ini 解析 / 数据模型 / 热加载           │
├───────────────────────────────────────────────┤
│ hook        WH_MOUSE_LL 钩子、触发状态机、右键还原  │
├───────────────────────────────────────────────┤
│ platform    Win32 薄封装、编码转换、工具函数        │
└───────────────────────────────────────────────┘
```

**数据流**（一次手势的生命周期）：

```
右键按下 → hook 捕获、记录目标窗口 → 进入 Tentative
   移动 → hook 采点入队 → 超过 MinDistance → 进入 Active，overlay 开始画线
   移动 → recognizer 增量量化方向、压缩序列 → overlay 更新方向串/动作名
右键松开 → recognizer 收尾匹配 → context 查 per-app → action 执行 → overlay 淡出
         └ 若未移动：hook 补发原生右键（弹出系统菜单）
```

---

## 4. 模块详细设计

### 4.1 hook — 输入捕获与触发状态机

`WH_MOUSE_LL` 的回调运行在**安装钩子的线程**上下文，由系统同步调用，**必须尽快返回**（超时会被系统摘掉钩子）。因此回调里只做"判定 + 采点入队"，绝不做绘制或 I/O。

**触发状态机：**

```
Idle ──(触发键 Down)──► Tentative ──(移动>MinDistance)──► Active
  ▲                        │                                 │
  │                        │(触发键 Up 且未移动)              │(触发键 Up)
  └────────────────────────┴────────────────►(执行/补发)─────┘
```

- **Down**：记录起点坐标，**此刻用 `GetForegroundWindow()` 锁定目标窗口**（不能等到 Up，焦点可能已变），置 Tentative，**返回非 0 吞掉此按下事件**（先扣住，不放行）。
- **Move**：距上一采样点 < `StepDistance` 则丢弃（去抖）；否则点入环形缓冲。累计位移首次超过 `MinDistance` → 置 Active，通知 overlay 开画。
- **Up**：
  - 若从未进入 Active（只是点击）→ **补发原生右键**：`SendInput` 合成一对 down/up，让目标程序弹出右键菜单。
  - 若 Active 且识别命中 → 交给 action 执行，吞掉。
  - 若 Active 但未匹配 → 吞掉，overlay 提示"未识别"。

**递归防护（关键坑）：** 我们 `SendInput` 补发的右键会再次进入 `WH_MOUSE_LL`。用两道判据放行自己的合成事件：
1. `MSLLHOOKSTRUCT.flags & LLMHF_INJECTED`；
2. 自定义签名 `dwExtraInfo == HUA_SIGNATURE`（如 `0x6875_0001`，取 'h''u'）。
命中任一即直接 `CallNextHookEx` 放行，不参与状态机。

**触发键映射**（`.ini` 的 `Trigger`）：`right→WM_RBUTTON*`、`middle→WM_MBUTTON*`、`x1/x2→WM_XBUTTON*`。中键/侧键路径**无需补发还原**（它们没有必须保留的原生菜单），实现更简单——这也是为什么把触发键做成可配。

### 4.2 recognizer — 方向识别（纯函数，重点可测）

**方向量化（8 方向九宫格）：**

```
输入：采样点序列 P[0..n]
对相邻点 (P[i], P[i+1]) 求 angle = atan2(dy, dx)
将 angle 量化到 8 个扇区，映射为九宫格数字：
    8=上  2=下  4=左  6=右
    7=左上 9=右上 1=左下 3=右下
```

**序列压缩（抗抖动）：** 连续相同方向合并为一个；新方向只有在该方向上**持续累计位移超过 `MinDistance`** 才被采纳，避免手抖产生噪声段。输出如 `"6"`、`"26"`、`"2141"`。

**匹配（编辑距离容错）：**
- 与手势表中每个 key（方向串）计算 **Levenshtein 距离**（标准 DP，串长 <10，开销可忽略）。
- 取距离最小且 `<= Tolerance` 者为命中；`Tolerance=0` 即精确匹配。
- 平票时优先更短模板 / ini 中更靠前者。
- 两段折线额外保留连续位移：明显向右展开（宽度不低于高度 30%），且两臂垂直高度相差不过分（短臂至少为长臂 40%）的下—上折线才按视觉 V 归一为 `39`。近似原路下—上的轨迹仍为独立的 `28`，向下后近水平向右的轨迹仍为 `26`。该规则确定、可解释，不依赖训练数据。

> 为什么用编辑距离而非精确相等：用户很难每次都画出完全一致的段数，容忍"多/少一段或方向偏一档"能极大提升可用性。参考 `avkom/gesture-recognition` 的 8 方向 + 编辑距离思路。

**接口（示意）：**

```c
// recognizer.h —— 无 Win32 依赖，纯逻辑，便于单测
typedef struct { int x, y; } Pt;

// 点序列 → 方向串（调用方提供输出缓冲）
size_t rec_encode(const Pt *pts, size_t n, int min_dist,
                  char *out, size_t out_cap);

// 编辑距离
int    rec_levenshtein(const char *a, const char *b);

// 在手势表中匹配，返回命中下标或 -1
int    rec_match(const char *seq, const char *const *keys,
                 size_t key_count, int tolerance);
```

**连贯多方向手势的准确率（重要）：** 方向序列法天生支持"一笔画、多转折"，一笔 `→↓←` 会被拆成 `624`。要让它稳，识别层必须做到三点：
1. **段内方向用累计位移向量判定**（该段起点→当前点），而非逐点 `atan2`——逐点会在方向边界因手抖反复横跳，拆出噪声段。
2. **转向滞回**：只有新方向上的累计位移超过 `MinDistance` 才确认为一次转折；短于阈值的段作为噪声抹掉。
3. **编辑距离容错**：吸收 ±1 段的多画/少画/偏一档误差。

实践边界：**2~5 段、方向拉得开的手势可稳定识别**；段数越多误判概率越累积；贴近 45° 对角边界的段易抖（该手势宜用正交方向或画得干脆）；圆弧/字母类连续曲线方向法不适用，需二期的 `$1` 模板匹配。

### 4.3 config — ini 数据模型与热加载

- **解析器用 inih（不自研）**：SAX 回调 `handler(user, section, name, value)`，在回调里增量构建模型——天然适配重复的 `[App:xxx]` 节与每节任意方向串作 key。编译期开关 `INI_ALLOW_MULTILINE`/`INI_ALLOW_INLINE_COMMENTS` 等按需打开；UTF-8 按字节透明，启动时剥掉可能的 BOM。
- 数据模型：`Config { General; Gesture[] global; App[] apps; }`，`App { name; Gesture[]; enabled; }`。
- **写回**：inih 只读；唯一需持久化的是托盘切换 `AutoStart`，写回那一行自行处理（或改用 minIni 的 `ini_puts` 内建写）。
- **热加载**：用 `ReadDirectoryChangesW` 或简单地在托盘菜单点"重载"时重读；一期先做手动重载 + 启动加载，二期加文件监听。
- 解析失败要**容错**：inih 返回出错行号，坏行跳过并记 log，不整体崩溃。

### 4.4 context — 前台程序识别与 per-app 解析

```c
// 取前台窗口所属进程的 exe 文件名（小写、不含路径）
bool ctx_foreground_exe(HWND hwnd, wchar_t *out, size_t cap);
// 链路：GetWindowThreadProcessId → OpenProcess(QUERY_LIMITED_INFORMATION)
//       → QueryFullProcessImageNameW → 取 basename → tolower
```

**全屏检测**（`DisableOnFullscreen=true` 时，触发前先判）：优先用 `SHQueryUserNotificationState()`，返回 `QUNS_RUNNING_D3D_FULL_SCREEN` 或 `QUNS_PRESENTATION_MODE` 视为全屏；再辅以几何比对——前台窗口矩形是否覆盖整块显示器且非桌面外壳（排除 `Progman`/`WorkerW`/任务栏），以覆盖无边框全屏游戏和网页全屏视频。命中则本次手势不触发。

**解析优先级**（识别出方向串 `seq`、目标窗口 `hwnd` 后）：

```
exe = ctx_foreground_exe(hwnd)
if DisableOnFullscreen 且 前台为全屏:         不触发
if FilterMode == whitelist 且 无 [App:exe]:  不触发
if [App:exe].enabled == false:               不触发（黑名单特例）
action = [App:exe].lookup(seq)  ?? [Gestures].lookup(seq)   // 程序覆盖 > 全局默认
```

> **设计取舍**：把"黑白名单"降级为 per-app 的特例（`Enabled=false` 或 whitelist 模式），避免两套并行概念。Per-app 映射本身比黑白名单更强——它决定"同手势不同动作"，而不仅是"生效与否"。全屏禁用是独立于名单的一道前置开关。

### 4.5 action — 动作执行

值的前缀约定：

| 前缀 | 语义 | 实现 |
|---|---|---|
| `key:` | 发送快捷键组合，如 `key:ctrl+shift+t` | 解析 modifier+主键 → `SendInput` 按下/抬起序列 |
| `run:` | 运行程序 / 打开文件，如 `run:C:\tools\a.exe` | `ShellExecuteW`（支持文件/URL）或 `CreateProcessW` |
| `cmd:` | 内置命令 | 硬编码命令表 |

**内置命令表（一期）：** `close_window`（向锁定的目标窗口发 `WM_CLOSE`）、`minimize`、`maximize`、`restore`、`toggle_maximize`（已最大化则还原，否则最大化）、`scroll_top`、`scroll_bottom`、`volume_up`、`volume_down`、`volume_mute`、`media_play`、`copy`、`paste`。

> 注意 `cmd:close_window` 操作的是**手势开始时锁定的目标窗口**，而非执行时的前台窗口——因为浮层/时序可能已让焦点变化。

### 4.6 overlay — 浮层渲染

- 一个全屏分层窗口：`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`。
  - `TRANSPARENT`：鼠标事件穿透到下层程序；`NOACTIVATE`：不抢焦点；`TOOLWINDOW`：不进任务栏/Alt-Tab。
- GDI+ 画折线（`SmoothingModeAntiAlias`），双缓冲防闪；线宽/颜色来自 ini。
- **动作名 OSD**：在轨迹末端或屏幕固定角落绘制 `方向串 + 命中动作名`（如 `26 → 关闭标签页`），随序列变化实时更新。
- **线程模型**：钩子回调只把点写入线程安全队列并 `PostMessage` 通知；overlay 的重绘发生在**主线程消息循环**里（定时器节流，如 60 FPS 上限），避免在钩子里绘制。
- 手势结束：清空并做 ~150ms 淡出。

### 4.7 app — 主程序

- 创建 message-only 窗口 + 托盘图标；托盘菜单：`重载配置` / `打开配置文件` / `开机自启（勾选态）` / `关于` / `退出`。
- 启动装钩子、加载配置、创建 overlay（初始隐藏）。
- 单实例：命名互斥量 `CreateMutexW`，重复启动则退出并前置提示。
- 退出：`UnhookWindowsHookEx`、`Shell_NotifyIcon(NIM_DELETE)`、GDI+ Shutdown。

**默认管理员权限运行：** manifest 设 `requestedExecutionLevel level="requireAdministrator"`。收益是钩子可覆盖提权窗口；代价是启动会过 UAC、且**默认无法接收来自非提权程序的拖放**（本工具不涉及拖放，无碍）。若日后想避免 UAC 又保留提权，可考虑签名 + UIAccess，但一期不做。

**开机自启（`AutoStart`）：** 因默认提权，**不用注册表 `Run` 键**（每次开机会弹 UAC）。改用**任务计划程序**：创建一个登录触发、勾选"以最高权限运行"的任务，即可静默提权自启。实现上启动时把 `AutoStart` 的值与计划任务状态**对账**（true 则创建/更新任务，false 则删除），托盘菜单的"开机自启"项切换同一状态。创建任务用 `ITaskService` COM 接口，或首版先调用 `schtasks.exe /create /rl highest /sc onlogon` 简化。

---

## 5. `.ini` 配置规范

保存为 **UTF-8**。方向数字：`8上 2下 4左 6右 / 7左上 9右上 1左下 3右下`。

```ini
; ===================== hua 配置 =====================

; 配置项对齐 MouseInc 的可调项（但仍纯 .ini、无 GUI）。
[General]
; --- 触发与识别 ---
Trigger         = right      ; right | middle | x1 | x2
TriggerDistance = 5          ; 按下后移动多远（px）才开始手势
MinDistance     = 20         ; 方向分段阈值（识别灵敏度，越小越灵敏）
StepDistance    = 12         ; 采点最小间隔像素（去抖）
Tolerance       = 0          ; 方向串匹配最大编辑距离（0=精确，本项目默认）
PauseTimeout    = 1000       ; 鼠标停顿超过此毫秒数则取消手势
FilterMode      = blacklist  ; blacklist | whitelist
DisableOnFullscreen = true   ; 全屏程序中禁用手势
AutoStart       = false      ; 开机自启（任务计划程序 + 最高权限）
RestoreEvent    = true       ; 未形成轨迹的识别失败是否补发原生按键

; --- 日志 ---
LogEnabled      = true       ; false 时完全不创建或写入日志
LogLevel        = warn       ; off | error | warn | info
LogMaxSizeMB    = 10         ; 达到上限后轮转为 hua-时间戳.log
LogRetentionDays= 2          ; 自动删除过期轮转日志

; --- 浮层外观（overlay/M5 消费） ---
ShowTrail       = true       ; 绘制过程显示轨迹线
ShowActionName  = true       ; 手势结束显示动作名
TrailArrow      = true       ; 轨迹末端画方向箭头
RandomColor     = false      ; 轨迹随机颜色
TrailColor      = 00A0FF     ; 手势颜色 RRGGBB
FailColor       = CAD0D3     ; 失败颜色 RRGGBB
TrailWidth      = 3          ; 轨迹线宽
TextSize        = 26         ; 动作名字号
TextPosition    = 150        ; 动作名距屏幕底部高度（px）

; ---------- 全局默认手势：方向串 = 动作（按九宫格数字升序） ----------
[Gestures]
1  = cmd:minimize         ; ↙ 左下    最小化
2  = cmd:scroll_bottom    ; ↓ 下      滚动到底部
3  = key:delete           ; ↘ 右下    删除
4  = key:alt+left         ; ← 左      后退
6  = key:alt+right        ; → 右      前进
7  = key:esc              ; ↖ 左上    Esc
8  = cmd:scroll_top       ; ↑ 上      滚动到顶部
9  = cmd:toggle_maximize  ; ↗ 右上    最大化 / 还原（切换）
26 = cmd:close_window     ; ↓→ 下右   关闭窗口

; 注意：2(向下) 与 26(下右) 为前缀关系。执行时按整体匹配不冲突，
; 但 Tolerance=1 时画不干脆可能互相误识别；追求零歧义可设 Tolerance=0。

; ---------- 程序专属覆盖 ----------
[App:chrome.exe]
26  = key:ctrl+w             ; 下·右：浏览器里只关当前标签页

[App:msedge.exe]
26  = key:ctrl+w

; ---------- 黑名单特例：该程序禁用手势 ----------
[App:game.exe]
Enabled = false
```

**如何"追加一个手势"**：在 `[Gestures]` 加一行 `方向串 = 动作`；要给某程序特殊化，就在对应 `[App:xxx.exe]` 里加同样的方向串行覆盖它。改完在托盘点"重载配置"即生效。

---

## 6. 目录结构

```
hua/
├─ CMakeLists.txt
├─ README.md
├─ config/
│  └─ hua.ini            # 默认配置，随发布包分发
├─ src/
│  ├─ main.c                  # app：入口 / 消息循环 / 托盘 / 单实例
│  ├─ hook.c  hook.h          # WH_MOUSE_LL、触发状态机、右键还原
│  ├─ recognizer.c .h         # 采点/量化/压缩/编辑距离（纯逻辑）
│  ├─ config.c .h             # ini 解析 / 数据模型 / 热加载
│  ├─ context.c .h            # 前台 exe / per-app 解析
│  ├─ action.c .h             # key / run / cmd
│  ├─ overlay.c .h            # 分层窗口 + GDI+ 绘制
│  └─ platform.c .h           # 编码转换 / 通用工具 / 日志
├─ third_party/
│  ├─ ini.c  ini.h            # inih（benhoyt/inih，New BSD）
│  └─ utest.h                 # 单头测试框架
├─ tests/
│  ├─ test_recognizer.c       # 方向串 / 编辑距离 / 匹配
│  └─ test_config.c           # ini 解析边界用例
└─ .github/workflows/
   └─ build.yml               # 构建 + 打包 + Release
```

---

## 7. 构建与发布

- **CMake**：`set(CMAKE_C_STANDARD 17)` + `set(CMAKE_C_STANDARD_REQUIRED ON)`；`add_executable(hua WIN32 ...)`（`WIN32` 去掉控制台窗口）；链接 `gdiplus user32 shell32 gdi32`。MSVC 用 `/std:c17`，MinGW 用 `-std=c17`。
- **测试**：`enable_testing()` + `add_test`；`recognizer`/`config` 编译为静态库供主程序与测试共用。
- **GitHub Actions**（`windows-latest`）：配置 → 构建 Release → 跑 CTest → 打包 `hua.exe + hua.ini` 为 zip → 打 tag 时发布 Release。与你现有的"GitHub Actions 构建 + Release 自动升级"流程一致。

```yaml
# build.yml 骨架
on: [push, pull_request]
jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - run: cmake -B build -DCMAKE_BUILD_TYPE=Release
      - run: cmake --build build --config Release
      - run: ctest --test-dir build -C Release --output-on-failure
      - uses: actions/upload-artifact@v4
        with: { name: hua, path: build/Release/hua.exe }
```

---

## 8. 单元测试计划

纯逻辑层是测试重点（无 Win32、可脱离系统运行）：

- **recognizer**
  - 点序列 → 方向串：直线各方向、L 形、Z 形、含抖动噪声点。
  - 编辑距离：对称性、空串、增删改各一步。
  - 匹配：精确命中、容错命中（距离=1）、超阈值不命中、平票取短。
- **config**
  - 正常解析、缺失可选项取默认、坏行跳过、UTF-8 中文动作名/路径、`[App:x]` 覆盖优先级、`Enabled=false`。
- **context / hook / overlay / action**：依赖系统 API，用手动冒烟测试清单（见下）覆盖，不做自动化单测。

**冒烟测试清单**（手动）：右键点击仍能弹出系统菜单（补发正确）；黑名单程序内手势不触发；per-app 覆盖在浏览器生效；高 DPI 下轨迹坐标正确；改 ini + 重载即时生效；SendInput 合成事件不被自身钩子二次处理。

---

## 9. 里程碑

> 状态（截至实现）：**M1–M6 全部完成**。纯逻辑层 51 个单元测试（recognizer/action/config）全绿；
> Win32 层（hook/context/overlay/action 执行/autostart）经手动冒烟。M7 未做（可选）。

1. ✅ **M1 骨架跑通**：托盘 + message-only 窗口 + 单实例 + 钩子装卸 + 日志。
2. ✅ **M2 识别闭环**：采点 → 方向串 → 精确匹配 → `cmd:`/`key:` 动作；右键补发还原。
3. ✅ **M3 配置化**：inih 解析 + 数据模型 + `Trigger`/阈值可配 + 托盘重载。
4. ✅ **M4 增强**：编辑距离容错 + per-app 映射 + 黑白名单 + 全屏门控。
5. ✅ **M5 浮层**：GDI+ 抗锯齿轨迹线 + 箭头 + 实时动作名 OSD + 淡出（自绘 flat C 声明）。
6. ✅ **M6 打磨**：热加载（`FindFirstChangeNotification`）、DPI 感知（PerMonitorV2）、开机自启（schtasks）、CI 发布、README。
7. **（可选）M7**：接 `htfy96/dollar` 的 `$1` 识别，支持任意形状手势。**（未做）**

---

## 10. 风险与注意事项

- **右键补发还原**是最易出 bug 处：合成事件递归、菜单弹出时机、目标程序对合成右键的兼容性——务必用签名 + `LLMHF_INJECTED` 双重放行，并保留原始坐标。
- **钩子性能**：回调必须微秒级返回，绝不在其中绘制/读文件；违反会被系统摘钩子且拖慢全局鼠标。
- **提权窗口**：已通过默认管理员运行解决（钩子可覆盖提权窗口）；副作用是启动过 UAC、默认不接收非提权程序拖放（本工具无碍）。
- **自启与 UAC**：提权程序切忌用注册表 `Run` 键自启（每次开机弹 UAC）——必须用任务计划程序 + 最高权限静默启动。
- **高 DPI / 多屏**：坐标需按 per-monitor DPI 处理；一期先声明 DPI 感知（manifest `PerMonitorV2`）保证主屏正确。全屏几何比对也要用 DPI 感知后的物理像素。
- **编码**：全程 UTF-8 文件 + UTF-16 内部，边界统一转码，杜绝中文乱码。

---

## 参考

- MouseInc 手册 — https://docs.shuax.com/MouseInc/
- Aitiy（MouseInc 作者新作） — https://aitiy.com/
- `$1` Unistroke Recognizer（华盛顿大学） — https://depts.washington.edu/acelab/proj/dollar/
- `htfy96/dollar`（C++17 的 `$1` 实现） — https://github.com/htfy96/dollar
- `avkom/gesture-recognition`（8 方向 + 编辑距离，可照抄算法） — https://github.com/avkom/gesture-recognition
- Easystroke（Linux C++ 手势工具，架构参考） — https://github.com/thjaeger/easystroke
```
