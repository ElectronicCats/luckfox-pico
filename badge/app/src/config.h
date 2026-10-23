/*
 * File: app/src/config.h
 * Purpose: Application configuration structure and limits.
 */

#ifndef BADGE_CONFIG_H
#define BADGE_CONFIG_H

#include <limits.h>
#include <stddef.h>

#define CONFIG_PATH_MAX 256

typedef struct app_config {
    char output_dir[CONFIG_PATH_MAX];
    char input_name[64];
    int width;
    int height;
    int quality;
    int led;
    int led_count;
    int capture_timeout_ms;
    int retries;
    int aiq;
    int fast_handoff;
    int log_level;
} app_config_t;

void config_defaults(app_config_t *cfg);
int config_parse_kv(app_config_t *cfg, const char *key, const char *value);
int config_load_file(app_config_t *cfg, const char *path);
int config_validate(const app_config_t *cfg);

#endif
