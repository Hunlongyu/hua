# Hua（划）· 鼠标手势工具开发设计文档

> **hua（划）** — "划一下"即手势的动作本身。Windows 平台、纯 C、无 GUI（仅托盘）、`.ini` 配置的鼠标手势工具。
> 目标是复刻 MouseInc / Aitiy 的**手势子集**，去掉截图、翻译、贴图等一切额外功能。

文档版本 v0.6 ｜ 平台 Windows 10/11 ｜ 语言 C17 ｜ 构建 CMake + GitHub Actions

---

## 0. 关键设计决策（本版定稿）

以下三点在此前讨论中未最终确认，本版按推荐默认值定稿，**均可在 `.ini` 或编译期改动**：

| 决策 | 选定方案 | 理由 |
|---|---|---|
| 触发键 | **右键**，`.ini` 可切 `middle/x1/x2` | 最贴近 MouseInc 体验；代价是要实现"右键补发还原" |
| 手势识别 | **连续几何联合评分 + 九宫格模板语言** | 配置简洁；原始角度、转角与形状信息不丢失 |
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
- 任意形状手势识别（画圆、画字母）——当前只支持九宫格方向模板；连续几何评分不会扩大模板语言的表达范围。
- 跨平台。仅 Windows。

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
| 单元测试 | `utest.h`（单头文件）+ CTest | 纯函数（几何识别、拒识、ini 解析、动作解析、探活判定）零依赖可测 |

**运行时依赖仅为 Windows 系统组件**（Win32、GDI+、WinHTTP、bcrypt 等）。inih、winautoupdate 及其 cJSON/semver 依赖均以源码静态编译；`utest.h` 只参与测试，不进入正式产物。无需 VC++ 运行库或其他第三方 DLL。

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
│ recognizer  RDP → 向量分段/迟滞 → 多特征评分 → 拒识   │
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
   移动 → hook 采点入队 → 超过 TriggerDistance → 进入 Active，overlay 开始画线
   移动 → 主线程快照原始点、几何匹配当前候选 → overlay 更新方向串/动作名
右键松开 → recognizer 最终匹配 → config 解析候选动作 → action 执行 → overlay 淡出
         └ 若未移动：hook 补发原生右键（弹出系统菜单）
```

---

## 4. 模块详细设计

### 4.1 hook — 输入捕获与触发状态机

`WH_MOUSE_LL` 的回调运行在**安装钩子的线程**上下文，由系统同步调用，**必须尽快返回**（超时会被系统摘掉钩子）。因此回调里只做"判定 + 采点入队"，绝不做绘制或 I/O。

**触发状态机：**

```
Idle ──(触发键 Down)──► Tentative ──(移动>TriggerDistance)──► Active
  ▲                        │                                 │
  │                        │(触发键 Up 且未移动)              │(触发键 Up)
  └────────────────────────┴────────────────►(执行/补发)─────┘
```

- **Down**：记录起点坐标，**此刻用 `GetForegroundWindow()` 锁定目标窗口**（不能等到 Up，焦点可能已变），置 Tentative，**返回非 0 吞掉此按下事件**（先扣住，不放行）。
- **Move**：距上一采样点 < `StepDistance` 则丢弃（去抖）；否则点入环形缓冲。起点净位移首次超过 `TriggerDistance` → 置 Active，通知 overlay 开画。
- **Up**：
  - 若从未进入 Active（只是点击）→ **补发原生右键**：`SendInput` 合成一对 down/up，让目标程序弹出右键菜单。
  - 若 Active 且识别命中 → 交给 action 执行，吞掉。
  - 若 Active 但未匹配 → 吞掉，overlay 提示"未识别"。

**递归防护（关键坑）：** 我们 `SendInput` 补发的右键会再次进入 `WH_MOUSE_LL`。用两道判据放行自己的合成事件：
1. `MSLLHOOKSTRUCT.flags & LLMHF_INJECTED`；
2. 自定义签名 `dwExtraInfo == HUA_SIGNATURE`（如 `0x6875_0001`，取 'h''u'）。
命中任一即直接 `CallNextHookEx` 放行，不参与状态机。

**触发键映射**（`.ini` 的 `Trigger`）：`right→WM_RBUTTON*`、`middle→WM_MBUTTON*`、`x1/x2→WM_XBUTTON*`。中键/侧键路径**无需补发还原**（它们没有必须保留的原生菜单），实现更简单——这也是为什么把触发键做成可配。

### 4.2 recognizer — 连续几何识别（纯函数，重点可测）

配置仍用九宫格方向串描述模板，但它只是模板语言；最终识别直接使用原始二维轨迹，
不再先量化成字符串。这样贴近扇区边界的一笔不会因为 1px 波动就彻底变成另一个手势。

**预处理与分段：**

1. 最多保留 256 个输入点，给热路径确定的时间/栈空间上界；轨迹另行按弧长重采样为 32 个形状点。
2. 用 Ramer–Douglas–Peucker 消除采样锯齿，容差由 `MinDistance × 0.42` 派生。
3. 相邻向量夹角小于 22.5°时合并，以整段净向量决定连续方向。
4. 新方向的确认距离为 `min(6×MinDistance, MinDistance + 上一段长度×0.10)`：前一笔越长，末端小钩越不容易被误判为转向。这一迟滞策略借鉴 FlowMouse 的实际实现。达到 `MinDistance` 但尚未通过确认的末段不会丢失，而作为暂定证据再做一次候选评分：若它使另一个已配置手势成为最佳，则拒绝执行稳定轨迹的短前缀；没有对应延伸候选时仍可把小钩当噪声。

**联合评分：** 对每个有效模板分别计算以下代价，再换算为 0～100 分：

- 42%：按双方线段长度比例对齐后的连续角度差；
- 18%：方向序列 DTW，吸收采样密度和转角位置差异；
- 20%：相对转角 DTW，区分方向集合相同但转弯结构不同的轨迹；
- 15%：起点平移、总弧长缩放后的 32 点整体形状差；
- 5%：有效段数差异。

不做旋转归一化，因为上/下/左/右本身就是动作语义；平移、速度、采样密度与整体尺寸不影响模板方向。
明显张开的两段 V/尖括号另有对称的拓扑置信度，以区分陡 V 与近似原路折返；开口宽度、深度和两臂平衡度连续改变加分，不再跨过单个像素阈值就固定跳变 18 分，且仍须通过几何最低分。

**拒识而不是猜测：**

- 最佳分必须达到 `MatchScore`（默认 82）；
- 最佳分必须领先第二名至少 `AmbiguityMargin`（默认 6）；
- 任何一项不满足都不执行动作。位于两个方向正中间的轨迹因此会被明确拒绝，而不是由取整偶然选中一边。

**接口（示意）：**

```c
typedef struct { int x, y; } Pt;
typedef struct { int index, score, second_score; } RecMatchResult;

size_t rec_normalize_template(const char *key, char *out, size_t out_cap);
bool rec_has_gesture(const Pt *pts, size_t n, int min_dist);
size_t rec_encode(const Pt *pts, size_t n, int min_dist,
                  char *out, size_t out_cap); // 仅供 OSD/日志
int rec_match_path(const Pt *pts, size_t n, int min_dist,
                   const char *const *keys, size_t key_count,
                   int min_score, int ambiguity_margin,
                   RecMatchResult *result);
```

实现使用固定大小栈数组，无堆分配、无训练过程、无模型文件和第三方运行库。实践目标仍是 1～5 段的方向型鼠标手势；圆、字母等自由曲线不在九宫格模板语言的表达范围内。

### 4.3 config — ini 数据模型与热加载

- **解析器用 inih（不自研）**：SAX 回调 `handler(user, section, name, value)`，在回调里增量构建模型。手势 key 只接受九宫格八方向，连续重复方向先折叠；同一节内标准化后重复的模板由最后定义覆盖并计入诊断。编译期开关 `INI_ALLOW_MULTILINE`/`INI_ALLOW_INLINE_COMMENTS` 等按需打开；UTF-8 按字节透明，启动时剥掉可能的 BOM。
- 数据模型：`Config { General; Gesture[] global; App[] apps; }`，`App { name; Gesture[]; enabled; }`。
- **写回**：inih 只读；唯一需持久化的是托盘切换 `AutoStart`，由纯文本编辑模块只改写对应行并原子替换配置文件。
- **热加载**：后台线程用 `FindFirstChangeNotificationW` 监听配置目录；收到通知后计算目标文件的内容指纹，只在 ini 实际变化时通知主线程重读，托盘菜单仍保留手动“重载配置”。
- 解析失败要**容错**：inih 返回出错行号，坏行跳过并记 log，不整体崩溃。

### 4.4 context — 前台程序识别与 per-app 解析

```c
// 取前台窗口所属进程的 exe 文件名（小写、不含路径）
bool ctx_foreground_exe(HWND hwnd, wchar_t *out, size_t cap);
// 链路：GetWindowThreadProcessId → OpenProcess(QUERY_LIMITED_INFORMATION)
//       → QueryFullProcessImageNameW → 取 basename → tolower
```

**全屏检测**（`DisableOnFullscreen=true` 时，触发前先判）：优先用 `SHQueryUserNotificationState()`，返回 `QUNS_RUNNING_D3D_FULL_SCREEN` 或 `QUNS_PRESENTATION_MODE` 视为全屏；再辅以几何比对——前台窗口矩形是否覆盖整块显示器且非桌面外壳（排除 `Progman`/`WorkerW`/任务栏），以覆盖无边框全屏游戏和网页全屏视频。命中则本次手势不触发。

**解析优先级**（取得原始轨迹 `points`、目标窗口 `hwnd` 后）：

```
exe = ctx_foreground_exe(hwnd)
if DisableOnFullscreen 且 前台为全屏:         不触发
if FilterMode == whitelist 且 无 [App:exe]:  不触发
if [App:exe].enabled == false:               不触发（黑名单特例）
candidates = [App:exe] + 未被同 key 覆盖的 [Gestures]
action = recognizer.match(points, candidates)               // 同一评分空间竞争
```

> **设计取舍**：把"黑白名单"降级为 per-app 的特例（`Enabled=false` 或 whitelist 模式），避免两套并行概念。Per-app 映射本身比黑白名单更强——它决定"同手势不同动作"，而不仅是"生效与否"。全屏禁用是独立于名单的一道前置开关。

### 4.5 action — 动作执行

值的前缀约定：

| 前缀 | 语义 | 实现 |
|---|---|---|
| `key:` | 发送快捷键组合，如 `key:ctrl+shift+t` | 解析 modifier+主键 → `SendInput` 按下/抬起序列 |
| `run:` | 运行程序 / 打开文件，如 `run:C:\tools\a.exe` | `ShellExecuteW`（支持文件/URL）或 `CreateProcessW` |
| `cmd:` | 内置命令 | 硬编码命令表 |

**内置命令表：** `close_window`（向锁定的目标窗口发 `WM_CLOSE`）、`minimize`、`maximize`、`restore`、`toggle_maximize`（已最大化则还原，否则最大化）、`scroll_top`、`scroll_bottom`、`volume_up`、`volume_down`、`volume_mute`、`media_play`、`copy`、`paste`、`open_exe_dir`。`cmd:none` 表示显式无动作，可在 per-app 中屏蔽全局手势且不回落。

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

**默认管理员权限运行：** manifest 设 `requestedExecutionLevel level="requireAdministrator"`。收益是钩子可覆盖提权窗口；代价是启动会过 UAC、且**默认无法接收来自非提权程序的拖放**（本工具不涉及拖放，无碍）。若日后想避免 UAC 又保留提权，可考虑签名 + UIAccess，当前不做。

**开机自启（`AutoStart`）：** 因默认提权，**不用注册表 `Run` 键**（每次开机会弹 UAC）。改用**任务计划程序**：创建一个登录触发、勾选“以最高权限运行”的任务，即可静默提权自启。实现上启动时把 `AutoStart` 的值与计划任务状态**对账**（true 则创建/更新任务，false 则删除），托盘菜单的“开机自启”项切换同一状态。任务通过 `ITaskService` COM 接口直接创建，不启动 `schtasks.exe`、不落临时 XML；任务设置为不受默认运行时长限制，并允许在电池供电时启动。

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
MatchScore      = 82         ; 几何匹配最低分（越高越严格）
AmbiguityMargin = 6          ; 最佳候选至少领先第二名多少分
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
FailColor       = 666666     ; 失败颜色 RRGGBB
TrailWidth      = 3          ; 轨迹线宽
TrailMaxLength  = 2500       ; 仅限制绘制的轨迹长度，不影响识别
TextSize        = 26         ; 动作名字号
TextPosition    = 150        ; 动作名距屏幕底部高度（px）
TextFillColor   = FFFFFF     ; 动作名填充色 RRGGBB
TextOutlineWidth= 3          ; 动作名描边宽度（px）
TextLetterSpacing = 4        ; 动作名字间距（px）

; --- 自动更新 ---
[Update]
Enabled         = true       ; 总开关；false 时不联网检查
AutoCheck       = true       ; 启动时后台检查，发现新版只提示
Channel         = stable     ; stable | beta

; ---------- 全局默认手势：方向串 = 动作（重复方向会折叠，非法/重复项写日志） ----------
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
39 = key:f5               ; ↘↗ V 形   刷新
86 = cmd:open_exe_dir     ; ↑→ 上右   打开 hua 所在目录

; 注意：2(向下) 与 26(下右) 为前缀关系。已达到 MinDistance 但尚未确认的末段
; 会作为候选意图参与评分，避免未画完 26 时先执行 2；候选过近时也会拒识。

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
│  ├─ main.c                  # app：入口 / 消息循环 / 托盘 / 单实例 / 配置监听
│  ├─ hook.c  hook.h          # WH_MOUSE_LL、触发状态机、右键还原
│  ├─ recognizer.c .h         # RDP/分段/DTW/形状评分与拒识（纯逻辑）
│  ├─ config.c .h             # ini 解析 / 数据模型 / 候选解析
│  ├─ context.c .h            # 前台 exe / per-app 解析
│  ├─ action.c .h             # key / run / cmd
│  ├─ overlay.c .h            # 分层窗口 + GDI+ 绘制
│  ├─ autostart.c .h          # 任务计划程序 COM 自启
│  ├─ update.c .h             # GitHub Release 自动更新
│  ├─ watchdog.c .h           # 钩子探活判定（纯逻辑）
│  └─ platform.c .h           # 编码转换 / 通用工具 / 日志
├─ third_party/
│  ├─ ini.c  ini.h            # inih（benhoyt/inih，New BSD）
│  └─ utest.h                 # 单头测试框架
├─ tests/
│  ├─ test_recognizer.c       # 分段 / 匹配 / 前缀安全 / 性质与负载回归
│  ├─ test_config.c           # ini 解析 / 标准化 / 候选解析边界
│  ├─ test_action.c           # 动作语法解析
│  ├─ test_watchdog.c         # 钩子探活判定
│  ├─ test_ini_edit.c         # AutoStart 配置写回
│  └─ test_logging.c          # 日志轮转与保留
└─ .github/workflows/
   ├─ ci.yml                  # 三架构构建、测试、PE/依赖校验
   └─ release.yml             # 标签校验、打包、校验和与 Release
```

---

## 7. 构建与发布

- **CMake**：`set(CMAKE_C_STANDARD 17)` + `set(CMAKE_C_STANDARD_REQUIRED ON)`；`add_executable(hua WIN32 ...)`（`WIN32` 去掉控制台窗口）；链接 `gdiplus user32 shell32 gdi32`。MSVC 用 `/std:c17`，MinGW 用 `-std=c17`。
- **测试**：`enable_testing()` + `add_test`；`recognizer`/`config` 编译为静态库供主程序与测试共用。
- **CI（`ci.yml`）**：main/master 推送与 PR 均构建 x64、x86、ARM64；x64/x86 执行 CTest，ARM64 在 x64 runner 上做编译校验；三者都校验 PE Machine 字段与静态 CRT（不得导入 VCRUNTIME/UCRT 转发 DLL）。
- **发布（`release.yml`）**：推送 `v*` 标签后先校验标签与 `src/version.h`；三架构重复构建、测试和依赖校验，再生成单文件 exe、含默认配置与许可证的 zip、`checksums.txt`，最后从 CHANGELOG 对应章节生成 Release 说明。带连字符的标签作为预发布。

---

## 8. 单元测试计划

纯逻辑层是测试重点（无 Win32、可脱离系统运行）：

- **recognizer**
  - RDP/迟滞分段：直线各方向、L/Z/V、短尾钩、含抖动噪声点，以及 `MinDistance` 到自适应确认距离的边界矩阵。
  - 前缀安全：已配置延伸候选时，未完成末段不得执行短前缀；未配置对应延伸时允许把小钩视为噪声。默认危险组合 `3=Delete` / `39=F5` 有配置层集成回归。
  - 几何匹配：尺寸/采样密度/平移不变性、方向边界歧义拒识、最低分拒识、V/原路折返拓扑连续扫描。
  - 232 组确定性随机变体：八个单方向与默认多段手势的角度、长度、密度、噪声矩阵。
  - 边界与负载：4096 输入点、192 候选的确定性结果，输出缓冲不足的原子失败，以及 `INT_MIN`/`INT_MAX` 坐标下分数有界。
  - 真实数据：目前只保留了 2026-07-17 日志中的一条 V 形轨迹作为回归；其余均明确为确定性合成样本。真实误识/漏识语料仍需随用户日志持续扩充。
- **config**
  - 正常解析、缺失可选项取默认、坏行跳过、UTF-8 中文动作名/路径、`[App:x]` 覆盖优先级、`Enabled=false`。
  - 全局与 app 模板统一做合法性校验、连续重复折叠、长度边界和“后定义覆盖前定义”诊断。
- **其他自动测试**：动作字符串解析、钩子 watchdog 纯逻辑、AutoStart 行写回、日志轮转/保留。
- **context / hook 安装与事件链 / overlay / action 系统调用 / autostart COM**：依赖 Windows 交互状态，继续由手动冒烟测试覆盖。

**冒烟测试清单**（手动）：右键点击仍能弹出系统菜单（补发正确）；黑名单程序内手势不触发；per-app 覆盖在浏览器生效；高 DPI 下轨迹坐标正确；改 ini + 重载即时生效；SendInput 合成事件不被自身钩子二次处理。

---

## 9. 里程碑

> 状态（截至实现）：**M1–M7 全部完成**。纯逻辑测试覆盖识别、配置、动作和系统无关辅助逻辑；
> Win32 层（hook/context/overlay/action 执行/autostart）经手动冒烟。

1. ✅ **M1 骨架跑通**：托盘 + message-only 窗口 + 单实例 + 钩子装卸 + 日志。
2. ✅ **M2 识别闭环**：采点 → 几何匹配 → `cmd:`/`key:` 动作；右键补发还原。
3. ✅ **M3 配置化**：inih 解析 + 数据模型 + `Trigger`/阈值可配 + 托盘重载。
4. ✅ **M4 增强**：per-app 映射 + 黑白名单 + 全屏门控。
5. ✅ **M5 浮层**：GDI+ 抗锯齿轨迹线 + 箭头 + 实时动作名 OSD + 淡出（自绘 flat C 声明）。
6. ✅ **M6 打磨**：热加载（`FindFirstChangeNotificationW`）、DPI 感知（PerMonitorV2）、开机自启（任务计划程序 COM）、CI 发布、README。
7. ✅ **M7 几何识别重构**：RDP + 自适应转向迟滞 + 连续角度/DTW/转角/形状联合评分 + 双阈值拒识。

---

## 10. 风险与注意事项

- **右键补发还原**是最易出 bug 处：合成事件递归、菜单弹出时机、目标程序对合成右键的兼容性——务必用签名 + `LLMHF_INJECTED` 双重放行，并保留原始坐标。
- **钩子性能**：回调必须微秒级返回，绝不在其中绘制/读文件；违反会被系统摘钩子且拖慢全局鼠标。
- **提权窗口**：已通过默认管理员运行解决（钩子可覆盖提权窗口）；副作用是启动过 UAC、默认不接收非提权程序拖放（本工具无碍）。
- **自启与 UAC**：提权程序切忌用注册表 `Run` 键自启（每次开机弹 UAC）——必须用任务计划程序 + 最高权限静默启动。
- **高 DPI / 多屏**：manifest 已声明 `PerMonitorV2`，输入、浮层与全屏几何比对统一使用物理像素；仍需在混合缩放比例、负坐标副屏和跨屏绘制场景做发布前冒烟。
- **编码**：全程 UTF-8 文件 + UTF-16 内部，边界统一转码，杜绝中文乱码。

---

## 参考

- MouseInc 手册 — https://docs.shuax.com/MouseInc/
- Aitiy（MouseInc 作者新作） — https://aitiy.com/
- Gesturefy（连续向量、比例匹配与 DTW 的浏览器手势实现） — https://github.com/Robbendebiene/Gesturefy
- FlowMouse（四方向与自适应转向阈值的浏览器手势实现） — https://github.com/Hmily-LCG/FlowMouse
- Chance-fyi/mouse-gestures（RDP、关键点与多特征匹配） — https://github.com/Chance-fyi/mouse-gestures
- Foxy Gestures（方向迟滞与非均匀扇区参考） — https://github.com/marklieberman/foxygestures
- Easystroke（Linux C++ 手势工具，架构参考） — https://github.com/thjaeger/easystroke
