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
