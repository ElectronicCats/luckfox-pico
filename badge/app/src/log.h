/*
 * File: app/src/log.h
 * Purpose: Log levels and logging helpers.
 */

#ifndef BADGE_LOG_H
#define BADGE_LOG_H

#include <stdarg.h>

enum log_level {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3,
};

void log_init(int level);
void log_set_level(int level);
int log_get_level(void);
void log_printf(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOGE(...) log_printf(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOGW(...) log_printf(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOGI(...) log_printf(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOGD(...) log_printf(LOG_LEVEL_DEBUG, __VA_ARGS__)

#endif
