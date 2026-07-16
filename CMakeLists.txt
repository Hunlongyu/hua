cmake_minimum_required(VERSION 3.20)
project(hua C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

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

# -------- 纯逻辑静态库（主程序与测试共用，零 Win32 依赖）--------
# inih（third_party/ini.c）作为源码内联编入，无运行时 DLL 依赖。
add_library(hua_core STATIC
  src/recognizer.c
  src/config.c
  third_party/ini.c
)
target_include_directories(hua_core PUBLIC src)
target_include_directories(hua_core PRIVATE third_party)

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
  src/hua.rc
)

target_include_directories(hua PRIVATE src)   # 同时供 windres 找到 app.manifest
target_link_libraries(hua PRIVATE hua_core gdiplus)

# 注：MinGW 本地构建时链接器会警告 "multiple non-default manifests"——因为
# gcc 驱动对 exe 无条件链入自带的 default-manifest.o，与我们 RC 里的 manifest
# 冲突。我们的 manifest 排在链接行更前故胜出（已验证 exe 内只含 requireAdministrator）。
# 该警告无害，且项目 CI 用 MSVC（经 mt.exe 处理 manifest），不会出现此警告。
target_link_libraries(hua PRIVATE user32 shell32 gdi32)

if(MSVC)
  target_compile_options(hua PRIVATE /W4)
  # hua.rc 已内嵌 requireAdministrator 清单，禁止链接器再生成同 ID 清单。
  target_link_options(hua PRIVATE /MANIFEST:NO)
else()
  target_compile_options(hua PRIVATE -Wall -Wextra)
endif()

# -------- 测试 --------
enable_testing()

# 测试框架用第三方 utest.h（single-header，仅测试代码依赖，不进生产二进制）。
add_executable(test_recognizer tests/test_recognizer.c)
target_link_libraries(test_recognizer PRIVATE hua_core)
target_include_directories(test_recognizer PRIVATE third_party)
add_test(NAME recognizer COMMAND test_recognizer)

# action 解析测试：编入 action.c + platform.c（用到 UTF-8 转换）。
add_executable(test_action tests/test_action.c src/action.c src/platform.c)
target_include_directories(test_action PRIVATE src third_party)
target_link_libraries(test_action PRIVATE user32 shell32)
add_test(NAME action COMMAND test_action)

# config 解析测试（config.c + inih 已在 hua_core 内）。
add_executable(test_config tests/test_config.c)
target_link_libraries(test_config PRIVATE hua_core)
target_include_directories(test_config PRIVATE third_party)
if(MSVC)
  # Config 含完整的 per-app 手势表，单个局部实例超过默认 1 MB 测试栈。
  target_link_options(test_config PRIVATE /STACK:4194304)
endif()
add_test(NAME config COMMAND test_config)
