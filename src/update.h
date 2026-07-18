/*
 * update.h —— hua 的自动更新（封装 third_party/updater 库）。
 *
 * hua 是托盘常驻程序，无控制台。更新检查在后台线程进行：
 *  - 启动静默检查（[Update] AutoCheck）：发现新版仅弹托盘气泡提示，不打扰。
 *  - 手动检查（托盘「检查更新」）：确认后下载并替换重启。
 *
 * 单实例握手与重启后清理由 update_boot() 承担，必须在创建单实例互斥量之前调用。
 */
#ifndef HUA_UPDATE_H
#define HUA_UPDATE_H

#include "config.h"
#include <windows.h>

/* 进程启动最早期调用（早于 CreateMutex）：若本次是更新后的重启，等旧进程退出并
 * 清理 .old 残留。非更新启动时是空操作。 */
void update_boot(void);

/* 启动时的后台静默检查。cfg.update_enabled && update_auto_check 时才真正联网。
 * 发现新版仅 PostMessage(WM_HUA_UPDATE) 请主线程弹气泡。 */
void update_start_background_check(HWND hwnd, const Config *cfg);

/* 托盘菜单「检查更新」：后台线程做完整流程（检查→确认→下载→替换重启）。 */
void update_check_now(HWND hwnd, const Config *cfg);

/* WM_HUA_UPDATE 处理：在主线程弹出「发现新版本」托盘气泡。 */
void update_on_found_message(HWND hwnd);

#endif /* HUA_UPDATE_H */
