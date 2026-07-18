cmake_minimum_required(VERSION 3.20)
project(hua C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 全程序优化（IPO/LTO → MSVC 的 /GL 编译 + /LTCG 链接），仅 Release。
# 跨翻译单元内联与全程序死代码消除；对静态 CRT 尤其有效——能裁掉库/CRT 里未被引用的
# 代码，是安全保留静态 CRT 的前提下压体积的主要手段。Debug 不启用（拖慢增量构建、碍调试）。
include(CheckIPOSupported)
check_ipo_supported(RESULT HUA_IPO_OK OUTPUT HUA_IPO_MSG LANGUAGES C)
if(HUA_IPO_OK)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
  message(STATUS "IPO/LTO 不可用，跳过：${HUA_IPO_MSG}")
endif()

# Release 追加「优先体积」/Os。默认 /O2 偏速度（/Ot），单开 LTCG 会因激进跨模块内联反而
# 增大产物（实测 +10KB）；本程序热路径极轻，用 /Os 换体积更划算。叠加上面的 LTCG 死代码
# 消除后，Release 体积约从 337KB 降到 312KB。/Os 排在 /O2 之后生效（覆盖 /Ot）。仅 Release。
if(MSVC)
  add_compile_options($<$<CONFIG:Release>:/Os>)
endif()

# 静态链接 CRT。两个理由：
# 1) 安全：VCRUNTIME140.dll / ucrtbase.dll 都不在 KnownDLLs 里，而 DLL 搜索顺序中
#    「exe 所在目录」先于 System32；它们在 WinMain 之前就被加载，任何运行期缓解
#    （SetDefaultDllDirectories 等）都来不及。本程序以管理员权限运行，一旦 exe 落在
#    用户可写目录（便携使用很常见），同名 DLL 即可被提权加载。静态链接直接消灭该导入。
#    注：gdiplus/user32/gdi32/shell32 在 KnownDLLs 中，不受此问题影响。
# 2) 部署：产物不再依赖 VC++ 运行库，便携分发更干净。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_language(RC)

# 全程宽字符 API。定义 UNICODE 让 MAKEINTRESOURCE / IDI_* 等宏解析为 W 版。
add_compile_definitions(UNICODE _UNICODE)
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)   # _snwprintf/_wfopen 等
  # 源文件与测试数据均为 UTF-8；所有目标都必须使用同一源字符集。
  add_compile_options(/utf-8)
endif()

# hua.rc 同为 UTF-8，但 RC 编译器不吃上面的 /utf-8：它默认按系统代码页（如 GBK）
# 解析 .rc，会把 VERSIONINFO 里的中文变成乱码。故显式指定代码页 65001。
if(MSVC)
  string(APPEND CMAKE_RC_FLAGS " /c65001")
else()
  string(APPEND CMAKE_RC_FLAGS " --codepage=65001")
endif()

# -------- 统一的警告等级 --------
# 用 INTERFACE 目标承载，好让自有代码的每个目标（含测试）都拿到同一套警告，
# 而 vendored 的 third_party 不受影响。
add_library(hua_warnings INTERFACE)
if(MSVC)
  target_compile_options(hua_warnings INTERFACE /W4)
else()
  target_compile_options(hua_warnings INTERFACE -Wall -Wextra)
endif()

# -------- 第三方 inih（源码内联，无运行时 DLL 依赖）--------
# 独立成 target：它是 vendored 代码，不该被我们的 /W4 刷屏，也不该为它改上游源码。
add_library(hua_inih STATIC third_party/ini.c)
target_include_directories(hua_inih PUBLIC third_party)
# 关掉多行值：本格式（key = value 单行）用不到它，开着反而有害——缩进行会被当成
# 上一个 key 的续行、用同名 key 再调一次 handler，于是 add_global_gesture 追加出
# 一条重复的手势（而非替换），白耗 CFG_MAX_GESTURES 槽位。
# PUBLIC：inih 是头文件里的 #ifdef 开关，消费者（config.c）编译时也必须看到同一定义。
target_compile_definitions(hua_inih PUBLIC INI_ALLOW_MULTILINE=0)

# -------- 第三方 winautoupdate（自动更新库，源码内联）--------
# 来源见 third_party/updater/VENDOR.md。独立 target：vendored 代码不受本项目 /W4 影响，
# 也不为 cJSON/semver 的既有告警改上游源码。仅链接 winhttp + bcrypt（系统库，非 VC 运行库，
# 不违反零依赖约定）。继承目录级的静态 CRT 设置（CMAKE_MSVC_RUNTIME_LIBRARY）。
file(GLOB HUA_UPDATER_SRC
  third_party/updater/src/*.c
  third_party/updater/src/third_party/*.c
)
add_library(hua_updater STATIC ${HUA_UPDATER_SRC})
target_include_directories(hua_updater
  PUBLIC  third_party/updater/include
  PRIVATE third_party/updater/src
)
target_link_libraries(hua_updater PUBLIC winhttp bcrypt)

# -------- 纯逻辑静态库（主程序与测试共用，零 Win32 依赖）--------
add_library(hua_core STATIC
  src/recognizer.c
  src/config.c
  src/watchdog.c
  src/ini_edit.c
)
target_include_directories(hua_core PUBLIC src)
target_link_libraries(hua_core PRIVATE hua_inih hua_warnings)

# -------- 主程序 --------
# WIN32 => Windows 子系统（无控制台窗口），入口走 WinMain。
add_executable(hua WIN32
  src/main.c
  src/hook.c
  src/action.c
  src/context.c
  src/overlay.c
  src/autostart.c
  src/platform.c
  src/update.c
  src/hua.rc
)

target_include_directories(hua PRIVATE src)   # 同时供 windres 找到 app.manifest
target_link_libraries(hua PRIVATE hua_core hua_updater gdiplus)

# 注：MinGW 本地构建时链接器会警告 "multiple non-default manifests"——因为
# gcc 驱动对 exe 无条件链入自带的 default-manifest.o，与我们 RC 里的 manifest
# 冲突。我们的 manifest 排在链接行更前故胜出（已验证 exe 内只含 requireAdministrator）。
# 该警告无害，且项目 CI 用 MSVC（经 mt.exe 处理 manifest），不会出现此警告。
target_link_libraries(hua PRIVATE user32 shell32 gdi32 hua_warnings)

if(MSVC)
  # hua.rc 已内嵌 requireAdministrator 清单，禁止链接器再生成同 ID 清单。
  target_link_options(hua PRIVATE /MANIFEST:NO)
endif()

# -------- 测试 --------
enable_testing()

# 测试框架用第三方 utest.h（single-header，仅测试代码依赖，不进生产二进制）。
add_executable(test_recognizer tests/test_recognizer.c)
target_link_libraries(test_recognizer PRIVATE hua_core hua_warnings)
target_include_directories(test_recognizer PRIVATE third_party)
add_test(NAME recognizer COMMAND test_recognizer)

# action 解析测试：编入 action.c + platform.c（用到 UTF-8 转换）。
add_executable(test_action tests/test_action.c src/action.c src/platform.c)
target_include_directories(test_action PRIVATE src third_party)
target_link_libraries(test_action PRIVATE user32 shell32 hua_warnings)
add_test(NAME action COMMAND test_action)

# 钩子探活判定测试（watchdog.c 在 hua_core 内，纯逻辑无 Win32 依赖）。
add_executable(test_watchdog tests/test_watchdog.c)
target_link_libraries(test_watchdog PRIVATE hua_core hua_warnings)
target_include_directories(test_watchdog PRIVATE third_party)
add_test(NAME watchdog COMMAND test_watchdog)

# ini AutoStart 改写测试（ini_edit.c 在 hua_core 内，纯文本变换无 Win32 依赖）。
add_executable(test_ini_edit tests/test_ini_edit.c)
target_link_libraries(test_ini_edit PRIVATE hua_core hua_warnings)
target_include_directories(test_ini_edit PRIVATE third_party)
add_test(NAME ini_edit COMMAND test_ini_edit)

# config 解析测试（config.c 在 hua_core 内）。
add_executable(test_config tests/test_config.c)
target_link_libraries(test_config PRIVATE hua_core hua_warnings)
target_include_directories(test_config PRIVATE third_party)
if(MSVC)
  # Config 含完整的 per-app 手势表，单个局部实例超过默认 1 MB 测试栈。
  target_link_options(test_config PRIVATE /STACK:4194304)
endif()
add_test(NAME config COMMAND test_config)

# 日志轮转测试必须与正式 hua.exe 隔离目录，避免测试改写用户正在查看的 hua.log。
add_executable(test_logging tests/test_logging.c src/platform.c)
target_include_directories(test_logging PRIVATE src third_party)
target_link_libraries(test_logging PRIVATE user32 shell32 hua_warnings)
set_target_properties(test_logging PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/log-test"
)
add_test(NAME logging COMMAND test_logging)
