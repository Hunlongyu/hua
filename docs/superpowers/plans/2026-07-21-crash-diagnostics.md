# hua Crash Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make hua 1.0.15 leave a bounded local crash summary, minidump, and previous-unclean-run warning without attempting unsafe recovery or automatic restart.

**Architecture:** Add a focused `crash_diag` Win32 module. Startup creates a versioned run marker and a dedicated dump-writer thread; the top-level exception filter only copies fixed-size exception state, signals that thread, waits at most eight seconds, and then preserves Windows' default unhandled-exception path. Normal cleanup removes the marker, while the next startup reports a leftover marker without claiming that every unclean stop was a crash.

**Tech Stack:** C17, Win32, DbgHelp `MiniDumpWriteDump`, CMake, MSVC, utest.h, CTest.

## Global Constraints

- Keep the application a single portable EXE with no new runtime DLL dependency.
- Do not automatically restart after a crash.
- Do not continue execution after an unhandled exception.
- Crash artifacts stay local and are never uploaded.
- Keep at most the newest 5 crash artifact pairs.
- The exception filter must not call `hua_logf`, allocate heap memory, or acquire the normal log lock.
- Return `EXCEPTION_CONTINUE_SEARCH` in production so Windows WER can still run.
- Bump the product patch version from 1.0.14 to 1.0.15.
- Use Chinese Conventional Commit messages and keep the title under 50 characters.

---

## File Structure

- Create `src/crash_diag.h`: small production API plus test-only hooks guarded by `HUA_CRASH_DIAG_TESTING`.
- Create `src/crash_diag.c`: directory resolution, run marker, retention, exception snapshot, writer thread, minidump, and shutdown.
- Create `tests/test_crash_diag.c`: parent-side lifecycle, filename, retention, and child-process assertions.
- Create `tests/crash_probe.c`: isolated child that intentionally raises access violation or stack overflow.
- Modify `src/main.c`: initialize before configuration parsing, report after logging starts, mark clean at the last normal cleanup point.
- Modify `src/version.h`: set patch version 15.
- Modify `CMakeLists.txt`: compile/link the module and two test executables with `Dbghelp`.
- Modify `README.md`: document crash artifact location and privacy.
- Modify `CHANGELOG.md`: add the 1.0.15 crash-diagnostics entry.

---

### Task 1: Run marker and previous-unclean-run detection

**Files:**
- Create: `src/crash_diag.h`
- Create: `src/crash_diag.c`
- Create: `tests/test_crash_diag.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `HUA_VERSION_STR` from `src/version.h` and Win32 file APIs.
- Produces: `bool crash_diag_init(void)`, `const char *crash_diag_previous_run(void)`, `void crash_diag_mark_clean_shutdown(void)`, and `void crash_diag_shutdown(void)`.
- Produces for tests only: `bool crash_diag_init_in_directory_for_test(const wchar_t *directory)` and `bool crash_diag_is_artifact_name_for_test(const wchar_t *name)` under `HUA_CRASH_DIAG_TESTING`.

- [ ] **Step 1: Add the public header and failing lifecycle tests**

Create `src/crash_diag.h` with this API:

```c
#ifndef HUA_CRASH_DIAG_H
#define HUA_CRASH_DIAG_H

#include <stdbool.h>
#include <windows.h>

bool crash_diag_init(void);
const char *crash_diag_previous_run(void);
void crash_diag_mark_clean_shutdown(void);
void crash_diag_shutdown(void);

#ifdef HUA_CRASH_DIAG_TESTING
bool crash_diag_init_in_directory_for_test(const wchar_t *directory);
bool crash_diag_is_artifact_name_for_test(const wchar_t *name);
#endif

#endif
```

Create `tests/test_crash_diag.c` with a PID-scoped directory built by `_snwprintf(dir, cap, L"%s\\hua-crash-test-%lu-%s", temp, GetCurrentProcessId(), case_name)` and these assertions:

```c
UTEST(crash_diag, clean_shutdown_removes_marker)
{
    wchar_t dir[MAX_PATH], marker[MAX_PATH];
    make_test_dir(dir, MAX_PATH, L"clean");
    join_path(marker, MAX_PATH, dir, L"hua-running.state");

    ASSERT_TRUE(crash_diag_init_in_directory_for_test(dir));
    ASSERT_TRUE(file_exists(marker));
    ASSERT_TRUE(crash_diag_previous_run() == NULL);
    crash_diag_mark_clean_shutdown();
    crash_diag_shutdown();
    ASSERT_FALSE(file_exists(marker));
    remove_test_dir(dir);
}

UTEST(crash_diag, leftover_marker_is_reported_without_calling_it_a_crash)
{
    wchar_t dir[MAX_PATH], marker[MAX_PATH];
    make_test_dir(dir, MAX_PATH, L"unclean");
    join_path(marker, MAX_PATH, dir, L"hua-running.state");
    write_utf8(marker, "format=1\r\nversion=1.0.14\r\npid=1234\r\nstarted=2026-07-21T08:30:00.000+08:00\r\n");

    ASSERT_TRUE(crash_diag_init_in_directory_for_test(dir));
    ASSERT_TRUE(strstr(crash_diag_previous_run(), "1.0.14") != NULL);
    ASSERT_TRUE(strstr(crash_diag_previous_run(), "1234") != NULL);
    crash_diag_mark_clean_shutdown();
    crash_diag_shutdown();
    remove_test_dir(dir);
}
```

The helpers use `CreateDirectoryW`, `CreateFileW`, `GetFileAttributesW`, and `RemoveDirectoryW`; no test writes outside its PID-scoped temp directory.

- [ ] **Step 2: Wire the test target and verify RED**

Add this CMake target before any implementation exists:

```cmake
add_executable(test_crash_diag tests/test_crash_diag.c src/crash_diag.c)
target_compile_definitions(test_crash_diag PRIVATE HUA_CRASH_DIAG_TESTING)
target_include_directories(test_crash_diag PRIVATE src third_party)
target_link_libraries(test_crash_diag PRIVATE dbghelp shell32 hua_warnings)
set_target_properties(test_crash_diag PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/crash-test"
)
add_test(NAME crash_diag COMMAND test_crash_diag)
```

Run:

```powershell
cmake --build build --config Debug --target test_crash_diag
```

Expected: link failure for the four undefined `crash_diag_*` functions.

- [ ] **Step 3: Implement the minimal marker lifecycle**

In `src/crash_diag.c`, use fixed global storage and no public mutable state:

```c
static wchar_t g_directory[MAX_PATH];
static wchar_t g_marker_path[MAX_PATH];
static char g_previous_run[256];
static bool g_initialized;
static bool g_clean;
```

`crash_diag_init_in_directory_for_test` must normalize a trailing backslash, create/read `hua-running.state`, read at most 511 bytes of the old marker, and parse only `version`, decimal `pid`, and `started`. Reject embedded control characters other than CR/LF separators; format `g_previous_run` as one line (`version=1.0.14 pid=1234 started=2026-07-21T08:30:00.000+08:00`) so a writable marker cannot inject extra log lines. Then format the new marker with this call. The `version` value comes from `HUA_VERSION_STR`, never from a second hard-coded version constant:

```c
int marker_len = snprintf(marker_text, sizeof(marker_text),
    "format=1\r\nversion=%s\r\npid=%lu\r\nstarted=%s\r\n",
    HUA_VERSION_STR, (unsigned long)GetCurrentProcessId(), iso_timestamp);
if (marker_len <= 0 || (size_t)marker_len >= sizeof(marker_text))
    return false;
```

Write `hua-running.state.tmp` with the exact open mode below, check every `WriteFile` result, close it, then call the atomic replacement:

```c
HANDLE file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
if (file == INVALID_HANDLE_VALUE)
    return false;

MoveFileExW(temp_path, g_marker_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
```

`crash_diag_mark_clean_shutdown` deletes only `g_marker_path`. `crash_diag_shutdown` closes resources but never deletes the marker by itself. `crash_diag_previous_run` returns `NULL` when no old marker was present.

Production `crash_diag_init` first obtains the EXE directory with `GetModuleFileNameW`, appends `crashes\`, creates it, and verifies writability by creating and deleting `hua-crash-write.probe`. If that fails, call `SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, local_dir)`, create `%LOCALAPPDATA%\Hua` and then `%LOCALAPPDATA%\Hua\CrashDumps`, and initialize in that directory. If both locations fail, return `false` without installing a filter or leaving a marker.

- [ ] **Step 4: Run the focused tests and verify GREEN**

Run:

```powershell
cmake --build build --config Debug --target test_crash_diag
ctest --test-dir build -C Debug -R '^crash_diag$' --output-on-failure
```

Expected: `100% tests passed, 0 tests failed`.

- [ ] **Step 5: Commit the marker deliverable**

```powershell
git add CMakeLists.txt src/crash_diag.h src/crash_diag.c tests/test_crash_diag.c
git commit -m "feat(crash): 记录异常结束的运行状态"
```

---

### Task 2: Strict crash artifact retention

**Files:**
- Modify: `src/crash_diag.c`
- Modify: `src/crash_diag.h`
- Modify: `tests/test_crash_diag.c`

**Interfaces:**
- Consumes: the initialized crash directory from Task 1.
- Produces for tests: `size_t crash_diag_prune_for_test(const wchar_t *directory)`.
- Produces internally: strict validation for names shaped like `hua-crash-20260721-091530-123-v1.0.15-p4321.txt` or the matching `.dmp`, and pruning by artifact stem.

Add the exact test-only declaration to `src/crash_diag.h`:

```c
size_t crash_diag_prune_for_test(const wchar_t *directory);
```

- [ ] **Step 1: Add failing matcher and retention tests**

Add these matcher assertions:

```c
ASSERT_TRUE(crash_diag_is_artifact_name_for_test(
    L"hua-crash-20260721-091530-123-v1.0.15-p4321.txt"));
ASSERT_TRUE(crash_diag_is_artifact_name_for_test(
    L"hua-crash-20260721-091530-123-v1.0.15-p4321.dmp"));
ASSERT_FALSE(crash_diag_is_artifact_name_for_test(L"hua-crash-notes.txt"));
ASSERT_FALSE(crash_diag_is_artifact_name_for_test(L"hua-crash-20260721-091530-123-v1.0.15-p4321.log"));
ASSERT_FALSE(crash_diag_is_artifact_name_for_test(L"important-hua-crash-20260721.dmp"));
```

Create six timestamped `.txt`/`.dmp` pairs and an unrelated `keep.txt`, call `crash_diag_prune_for_test`, then assert that the newest five pairs and `keep.txt` remain while both files in the oldest pair are gone.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
cmake --build build --config Debug --target test_crash_diag
ctest --test-dir build -C Debug -R '^crash_diag$' --output-on-failure
```

Expected: matcher/retention assertions fail because pruning is not implemented.

- [ ] **Step 3: Implement strict grouping and pruning**

Accept only names whose prefix is exactly `hua-crash-`, whose final extension is exactly `.txt` or `.dmp`, whose date/time fields contain only digits in fixed positions, and whose `-v` version and `-p` PID fields are non-empty. Strip the extension to form the artifact stem.

Enumerate `hua-crash-*` with `FindFirstFileW`, group matching files by stem in a fixed array of 64 entries, retain the newest `FILETIME` for each stem, sort descending, and delete `.txt` plus `.dmp` only for stems after index 4. If more than 64 stems exist, make additional passes that always delete the globally oldest matching stem until five remain; never broaden the filename pattern.

Call pruning once after directory initialization and once after a crash artifact pair is completed.

- [ ] **Step 4: Run focused and existing logging tests**

Run:

```powershell
cmake --build build --config Debug --target test_crash_diag test_logging
ctest --test-dir build -C Debug -R '^(crash_diag|logging)$' --output-on-failure
```

Expected: both tests pass and `keep.txt` survives.

- [ ] **Step 5: Commit retention**

```powershell
git add src/crash_diag.h src/crash_diag.c tests/test_crash_diag.c
git commit -m "feat(crash): 限制本地转储保留数量"
```

---

### Task 3: Dedicated crash writer and minidump probe

**Files:**
- Modify: `src/crash_diag.c`
- Modify: `src/crash_diag.h`
- Modify: `tests/test_crash_diag.c`
- Create: `tests/crash_probe.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the crash directory, matcher, and pruning from Tasks 1-2.
- Produces: a top-level exception filter plus writer-thread lifecycle hidden inside `crash_diag_init`/`crash_diag_shutdown`.
- Produces for tests: `void crash_diag_suppress_wer_for_test(bool suppress)` so the deliberate crash does not create machine-wide WER UI or extra dumps.

Add the exact test-only declaration to `src/crash_diag.h`:

```c
void crash_diag_suppress_wer_for_test(bool suppress);
```

- [ ] **Step 1: Add the crash probe and failing parent assertions**

`tests/crash_probe.c` accepts mode `--av` or `--stack` in `argv[1]` and the concrete output directory in `argv[2]`, calls `SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS)`, initializes diagnostics in that directory, enables test-only WER suppression, and crashes:

```c
__declspec(noinline) static void raise_access_violation(void)
{
    volatile int *p = (volatile int *)0;
    *p = 1;
}

__declspec(noinline) static void overflow_stack(unsigned depth)
{
    volatile unsigned char page[8192];
    page[depth & 4095] = (unsigned char)depth;
    overflow_stack(depth + 1);
    ((volatile unsigned char *)page)[0] = page[depth & 4095];
}
```

The parent test starts the probe with `CreateProcessW`, waits at most 15 seconds, asserts a nonzero exit code, and verifies within the supplied directory:

```c
ASSERT_EQ(count_pattern(dir, L"hua-crash-*.txt"), 1);
ASSERT_EQ(count_pattern(dir, L"hua-crash-*.dmp"), 1);
ASSERT_TRUE(first_matching_file_size(dir, L"hua-crash-*.dmp") > 0);
ASSERT_TRUE(summary_contains(dir, "exception_code=0xC0000005"));
ASSERT_TRUE(file_exists(marker));
```

Repeat with `--stack`, expecting `exception_code=0xC00000FD`.

- [ ] **Step 2: Add the probe target and verify RED**

Add:

```cmake
add_executable(crash_probe tests/crash_probe.c src/crash_diag.c)
target_compile_definitions(crash_probe PRIVATE HUA_CRASH_DIAG_TESTING)
target_include_directories(crash_probe PRIVATE src)
target_link_libraries(crash_probe PRIVATE dbghelp shell32 hua_warnings)
set_target_properties(crash_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/crash-test"
)
add_dependencies(test_crash_diag crash_probe)
```

Run the focused CTest. Expected: the child exits but no summary or dump exists.

- [ ] **Step 3: Implement the exception snapshot and writer thread**

Add fixed global exception state:

```c
static HANDLE g_crash_event;
static HANDLE g_crash_done_event;
static HANDLE g_stop_event;
static HANDLE g_writer_thread;
static LONG g_crash_claimed;
static DWORD g_fault_thread_id;
static EXCEPTION_RECORD g_exception_record;
static CONTEXT g_context;
static EXCEPTION_POINTERS g_exception_pointers = {
    &g_exception_record, &g_context
};
static LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter;
static bool g_test_suppress_wer;
```

The filter must contain only fixed-size copies, interlocked claim, event signaling, and bounded wait:

```c
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_crash_claimed, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    g_fault_thread_id = GetCurrentThreadId();
    g_exception_record = *ep->ExceptionRecord;
    g_context = *ep->ContextRecord;
    SetEvent(g_crash_event);
    WaitForSingleObject(g_crash_done_event, 8000);

    if (g_test_suppress_wer)
        return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}
```

The writer thread waits for either crash or stop. On crash it builds one filename stem from `GetLocalTime`, `HUA_VERSION_STR`, and `GetCurrentProcessId`; opens the `.dmp` with `CREATE_NEW`; and calls:

```c
MINIDUMP_EXCEPTION_INFORMATION mei;
mei.ThreadId = g_fault_thread_id;
mei.ExceptionPointers = &g_exception_pointers;
mei.ClientPointers = FALSE;

BOOL dump_ok = MiniDumpWriteDump(
    GetCurrentProcess(), GetCurrentProcessId(), dump_file,
    MiniDumpNormal | MiniDumpScanMemory | MiniDumpWithThreadInfo |
        MiniDumpWithIndirectlyReferencedMemory,
    &mei, NULL, NULL);
DWORD dump_error = dump_ok ? ERROR_SUCCESS : GetLastError();
```

It then writes a UTF-8 `.txt` via `CreateFileW`/`WriteFile`, formatting the exact keys with:

```c
int summary_len = snprintf(summary, sizeof(summary),
    "format=1\r\nversion=%s\r\npid=%lu\r\nthread_id=%lu\r\n"
    "exception_code=0x%08lX\r\nexception_address=0x%016llX\r\n"
    "exe=%s\r\ndump_ok=%s\r\ndump_error=%lu\r\n",
    HUA_VERSION_STR,
    (unsigned long)GetCurrentProcessId(),
    (unsigned long)g_fault_thread_id,
    (unsigned long)g_exception_record.ExceptionCode,
    (unsigned long long)(uintptr_t)g_exception_record.ExceptionAddress,
    exe_utf8,
    dump_ok ? "true" : "false",
    (unsigned long)dump_error);
```

Obtain the EXE path into fixed `wchar_t exe_path[MAX_PATH]` storage and convert it without allocation:

```c
char exe_utf8[MAX_PATH * 3];
int exe_len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                  exe_path, -1, exe_utf8,
                                  (int)sizeof(exe_utf8), NULL, NULL);
if (exe_len <= 0)
    strcpy_s(exe_utf8, sizeof(exe_utf8), "<path unavailable>");
```

Do not allocate memory in the writer thread. Include `<stdint.h>` for the `uintptr_t` conversion above.

Signal `g_crash_done_event` after both files have been closed. Do not use `hua_logf` anywhere on this path.

During init, create all events and the thread before installing the filter. Call `SetThreadStackGuarantee` with 64 KiB on the main thread. During shutdown, restore the previous filter, signal the stop event, wait up to five seconds, and close every handle.

- [ ] **Step 4: Run access-violation and stack-overflow probes**

Run:

```powershell
cmake --build build --config Debug --target test_crash_diag crash_probe
ctest --test-dir build -C Debug -R '^crash_diag$' --output-on-failure
```

Expected: both probe modes produce one nonempty dump, correct exception code, and a leftover marker; the parent removes only its temp directory afterward.

- [ ] **Step 5: Commit crash capture**

```powershell
git add CMakeLists.txt src/crash_diag.h src/crash_diag.c tests/test_crash_diag.c tests/crash_probe.c
git commit -m "feat(crash): 保存异常摘要和小型转储"
```

---

### Task 4: Application integration, version, and user documentation

**Files:**
- Modify: `src/main.c:13-21, 985-1120`
- Modify: `src/version.h:10-12`
- Modify: `CMakeLists.txt:102-139`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all production functions from `src/crash_diag.h`.
- Produces: hua 1.0.15 with crash diagnostics active before configuration parsing and clean marker removal after all ordinary cleanup.

- [ ] **Step 1: Add a failing production-link integration**

Add `src/crash_diag.c` to the `hua` source list and link `dbghelp`. Build before editing `main.c`:

```cmake
add_executable(hua WIN32
  src/main.c
  src/crash_diag.c
  src/hook.c
  src/action.c
  src/context.c
  src/overlay.c
  src/autostart.c
  src/platform.c
  src/update.c
  src/hua.rc
)
target_link_libraries(hua PRIVATE hua_core hua_updater gdiplus dbghelp)
```

Run `cmake --build build --config Debug --target hua`. Expected: build succeeds but no production call references the module yet; this establishes link coverage before lifecycle integration.

- [ ] **Step 2: Integrate startup, warning, and final clean shutdown**

Include `crash_diag.h`. Immediately after the mutex `ERROR_ALREADY_EXISTS` branch, before `configure_log_before_init`, add:

```c
bool crash_diag_ready = crash_diag_init();
configure_log_before_init();
hua_log_init();
if (!crash_diag_ready) {
    HUA_LOG_W("崩溃诊断初始化失败；本次异常可能无法生成转储");
} else {
    const char *previous = crash_diag_previous_run();
    if (previous) {
        HUA_LOG_W("上次运行未正常结束（可能是崩溃、强制终止、断电或系统重启）：%s",
                  previous);
    }
}
```

At `cleanup_mutex`, keep diagnostics active through every cleanup and through `hua_log_close`, then add:

```c
hua_log_close();
if (crash_diag_ready) {
    crash_diag_mark_clean_shutdown();
    crash_diag_shutdown();
}
```

Initialize `crash_diag_ready` before any `goto cleanup_mutex` can cross its declaration, so every normal early-return cleanup deletes the marker and closes the worker.

- [ ] **Step 3: Bump version and document local artifacts**

Set `HUA_VER_PATCH` to `15`. Add a README troubleshooting paragraph stating both directories, the five-pair limit, and that `.dmp` can contain local process memory fragments and is never uploaded automatically. Add a `1.0.15` CHANGELOG section describing the unclean-run warning, exception summary, minidump, and no automatic restart.

- [ ] **Step 4: Run Debug tests and production smoke test**

Run:

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: every test passes. Start `build\Debug\hua.exe`, verify a single responsive `hua` process and tray icon, exit from the tray menu, and verify `build\Debug\crashes\hua-running.state` does not remain.

- [ ] **Step 5: Commit integration**

```powershell
git add CMakeLists.txt src/main.c src/version.h README.md CHANGELOG.md
git commit -m "feat(crash): 接入崩溃诊断并升级到 1.0.15"
```

---

### Task 5: Full verification and installed-binary handoff

**Files:**
- Verify only; do not change source unless a failing test identifies a root cause.

**Interfaces:**
- Consumes: hua 1.0.15 and all tests from Tasks 1-4.
- Produces: verified Debug and Release artifacts plus a recoverable installed-binary update.

- [ ] **Step 1: Reconfigure with the discovered Visual Studio Insiders toolchain**

Run:

```powershell
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw '未找到 Visual Studio C++ x64 工具链' }
cmake -S . -B build -A x64
```

Expected: configure succeeds and reports the MSVC compiler from the discovered Insiders installation.

- [ ] **Step 2: Run complete Debug and Release verification**

Run:

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected: both configurations report `100% tests passed`.

- [ ] **Step 3: Inspect the Release artifact**

Run:

```powershell
(Get-Item '.\build\Release\hua.exe').VersionInfo |
  Select-Object FileVersion,ProductVersion,FileName
Get-Item '.\build\Release\hua.exe' | Select-Object Length,LastWriteTime
```

Expected: file and product version are both `1.0.15`, and the timestamp belongs to this build.

- [ ] **Step 4: Update the installed copy recoverably and restart**

Resolve and verify that the only running `hua` process image is `D:\Software\ciqtek\hua\hua.exe`. Stop that exact process, copy the current installed EXE to `D:\Software\ciqtek\hua\hua-1.0.14.backup.exe`, copy `build\Release\hua.exe` to `D:\Software\ciqtek\hua\hua.exe`, and restart it hidden. Do not touch `hua.ini` or `hua.log`.

After five seconds verify one responsive `hua` process whose file version is 1.0.15. Verify `D:\Software\ciqtek\hua\crashes\hua-running.state` exists while running. Exit normally from the tray when testing clean removal; restart once more for the user's continued use.

- [ ] **Step 5: Final diff and status audit**

Run:

```powershell
git diff --check
git status --short
git log -5 --oneline
```

Expected: no whitespace errors, no uncommitted source changes, and the three implementation commits plus the design/plan commits appear in history.
