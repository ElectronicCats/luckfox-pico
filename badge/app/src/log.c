/*
 * File: app/src/log.c
 * Purpose: Leveled logging to stderr and the kernel log.
 */

#include "log.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_level = LOG_LEVEL_INFO;
static int g_kmsg_fd = -1;

void log_init(int level)
{
    g_level = level;
    if (g_kmsg_fd < 0)
        g_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
}

void log_set_level(int level)
{
    if (level < LOG_LEVEL_ERROR)
        level = LOG_LEVEL_ERROR;
    if (level > LOG_LEVEL_DEBUG)
        level = LOG_LEVEL_DEBUG;
    g_level = level;
}

int log_get_level(void)
{
    return g_level;
}

void log_printf(int level, const char *fmt, ...)
{
    static const char *names[] = { "E", "W", "I", "D" };
    char buf[1024];
    va_list ap;
    int n;

    if (level > g_level)
        return;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;

    dprintf(STDERR_FILENO, "[badge_photo][%s] %s%s",
            names[level < 0 ? 0 : (level > 3 ? 3 : level)],
            buf, (n > 0 && buf[n - 1] == '\n') ? "" : "\n");

    if (g_kmsg_fd >= 0) {
        char kbuf[1100];
        int k = snprintf(kbuf, sizeof(kbuf), "badge_photo: %s", buf);
        if (k > 0) {
            ssize_t ignored = write(g_kmsg_fd, kbuf, (size_t)k);
            (void)ignored;
        }
    }
}
