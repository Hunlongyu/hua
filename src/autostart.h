/*
 * autostart.h —— 开机自启（任务计划程序，最高权限，静默）。
 * 因程序默认提权运行，不能用注册表 Run 键（每次开机弹 UAC），改用登录触发的
 * 计划任务（勾选「以最高权限运行」）静默提权自启。
 */
#ifndef HUA_AUTOSTART_H
#define HUA_AUTOSTART_H

#include <stdbool.h>

/* 创建/删除登录自启任务。成功返回 true。 */
bool autostart_set(bool enable);

/* 查询自启任务是否已存在。 */
bool autostart_exists(void);

#endif /* HUA_AUTOSTART_H */
