/*
 * File: app/src/meta.c
 * Purpose: Fastboot meta partition mapping and app/cmdline parameter parsing.
 */

#include "meta.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rk_meta.h"
#include "rk_meta_app_param.h"
#include "sensor_init_info.h"
#include "sensor_iq_info.h"

int cmdline_get_value(const char *cmdline, const char *key, long *out)
{
    const char *p;
    size_t keylen;

    if (!cmdline || !key || !out)
        return 0;

    keylen = strlen(key);
    p = cmdline;

    while ((p = strstr(p, key)) != NULL) {
        const char *v;
        char *end = NULL;
        long value;

        /* Require token boundary before the key. */
        if (p != cmdline && p[-1] != ' ' && p[-1] != '\t') {
            p += keylen;
            continue;
        }
        v = p + keylen;
        if (*v != '=') {
            p += keylen;
            continue;
        }
        v++;
        errno = 0;
        value = strtol(v, &end, 0);
        if (errno == 0 && end != v) {
            *out = value;
            return 1;
        }
        p += keylen;
    }
    return 0;
}

static int read_cmdline(char *buf, size_t size)
{
    int fd;
    ssize_t n;

    if (!buf || size == 0)
        return -1;
    fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

int meta_open(badge_meta_t *meta)
{
    char cmdline[1024];
    long addr = 0x800000;
    long size = 0x60000;
    long iq_size = 0;
    long page = sysconf(_SC_PAGESIZE);
    long aligned;
    size_t delta;
    void *map;

    if (!meta)
        return -1;
    memset(meta, 0, sizeof(*meta));
    meta->mem_fd = -1;

    if (read_cmdline(cmdline, sizeof(cmdline)) != 0) {
        LOGE("no se pudo leer /proc/cmdline\n");
        return -1;
    }

    cmdline_get_value(cmdline, "meta_load_addr", &addr);
    cmdline_get_value(cmdline, "meta_part_size", &size);
    cmdline_get_value(cmdline, "rk_iqbin_size", &iq_size);

    if (addr <= 0 || size <= 0 || page <= 0) {
        LOGE("meta: parámetros inválidos addr=0x%lx size=0x%lx\n", addr, size);
        return -1;
    }

    aligned = addr & ~(page - 1);
    delta = (size_t)(addr - aligned);

    meta->mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (meta->mem_fd < 0) {
        LOGE("meta: no se pudo abrir /dev/mem: %s\n", strerror(errno));
        return -1;
    }

    map = mmap(NULL, (size_t)size + delta, PROT_READ | PROT_WRITE,
               MAP_SHARED, meta->mem_fd, (off_t)aligned);
    if (map == MAP_FAILED) {
        LOGE("meta: mmap falló: %s\n", strerror(errno));
        close(meta->mem_fd);
        meta->mem_fd = -1;
        return -1;
    }

    meta->map = map;
    meta->map_size = (size_t)size + delta;
    meta->base = (char *)map + delta;
    meta->size = (size_t)size;
    meta->app = (struct app_param_info *)((char *)meta->base + APP_PARAM_OFFSET);
    meta->sensor = (struct sensor_init_cfg *)((char *)meta->base + SENSOR_INIT_OFFSET);
    meta->iq = (char *)meta->base + SENSOR_IQ_BIN_OFFSET +
               offsetof(struct sensor_iq_info, data);
    meta->iq_size = iq_size > 0 ? (size_t)iq_size : SENSOR_IQ_BIN_MAX_SIZE;

    LOGI("meta: addr=0x%lx size=0x%lx iq=%zu cam=%dx%d\n",
         addr, size, meta->iq_size,
         meta->sensor ? meta->sensor->cam_w : 0,
         meta->sensor ? meta->sensor->cam_h : 0);
    return 0;
}

void meta_close(badge_meta_t *meta)
{
    if (!meta)
        return;
    if (meta->map && meta->map != MAP_FAILED)
        munmap(meta->map, meta->map_size);
    if (meta->mem_fd >= 0)
        close(meta->mem_fd);
    memset(meta, 0, sizeof(*meta));
    meta->mem_fd = -1;
}
