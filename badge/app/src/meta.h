/*
 * File: app/src/meta.h
 * Purpose: Fastboot meta mapping and parameter access interface.
 */

#ifndef BADGE_META_H
#define BADGE_META_H

#include <stddef.h>

struct app_param_info;
struct sensor_init_cfg;

typedef struct badge_meta {
    int mem_fd;
    void *map;
    size_t map_size;
    void *base;
    size_t size;
    struct app_param_info *app;
    struct sensor_init_cfg *sensor;
    void *iq;
    size_t iq_size;
} badge_meta_t;

/* Parse a key=value token from a cmdline string. Returns 1 if found. */
int cmdline_get_value(const char *cmdline, const char *key, long *out);

int meta_open(badge_meta_t *meta);
void meta_close(badge_meta_t *meta);

#endif
