/*
 * File: app/src/photo_path.c
 * Purpose: Photo filename generation, directory creation, and MTP-safe staging paths.
 */

#include "photo_path.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int join_path(char *out, size_t outsz, const char *dir, const char *name)
{
    int n;

    if (!out || outsz == 0 || !dir || !name)
        return -1;
    n = snprintf(out, outsz, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= outsz)
        return -1;
    return 0;
}

int photo_path_format(const char *dir, time_t epoch, unsigned int seq,
                      char *out, size_t outsz)
{
    char name[96];
    struct tm tm;
    int n;

    if (!dir || !out || outsz == 0)
        return -1;

    if (epoch > 0 && localtime_r(&epoch, &tm)) {
        char stamp[32];
        if (strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm) == 0)
            return -1;
        n = snprintf(name, sizeof(name), "IMG_%s_%04u.jpg", stamp, seq % 10000);
    } else {
        n = snprintf(name, sizeof(name), "IMG_boot_%04u.jpg", seq % 10000);
    }

    if (n < 0 || (size_t)n >= sizeof(name))
        return -1;
    return join_path(out, outsz, dir, name);
}

int photo_path_make_temp(const char *path, const char *staging_dir,
                         char *out, size_t outsz)
{
    const char *base;
    int n;

    if (!path || !staging_dir || !out || outsz == 0)
        return -1;

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (base[0] == '\0')
        return -1;

    n = snprintf(out, outsz, "%s/%s.tmp", staging_dir, base);
    if (n < 0 || (size_t)n >= outsz)
        return -1;
    return 0;
}

int photo_path_next(const char *dir, char *out, size_t outsz)
{
    time_t now;
    unsigned int seq;

    if (!dir || !out || outsz == 0)
        return -1;

    now = time(NULL);
    for (seq = 0; seq < 10000; seq++) {
        if (photo_path_format(dir, now, seq, out, outsz) != 0)
            return -1;
        if (access(out, F_OK) != 0 && errno == ENOENT)
            return 0;
    }
    return -1;
}

int photo_path_ensure_dir(const char *dir)
{
    char tmp[512];
    size_t len;
    char *p;

    if (!dir || dir[0] == '\0')
        return -1;

    len = strlen(dir);
    if (len >= sizeof(tmp))
        return -1;
    memcpy(tmp, dir, len + 1);

    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int photo_path_clean_staging(const char *staging_dir)
{
    DIR *dir;
    struct dirent *entry;
    char path[512];
    int removed = 0;

    if (!staging_dir)
        return -1;

    dir = opendir(staging_dir);
    if (!dir)
        return errno == ENOENT ? 0 : -1;

    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        int n;

        if (len < 4 || strcmp(entry->d_name + len - 4, ".tmp") != 0)
            continue;
        n = snprintf(path, sizeof(path), "%s/%s", staging_dir,
                     entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        if (unlink(path) == 0)
            removed++;
    }

    closedir(dir);
    return removed;
}
