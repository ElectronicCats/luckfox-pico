/*
 * File: app/src/config.c
 * Purpose: Default configuration and key=value configuration file loader.
 */

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_string(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, dstsz, "%s", src);
}

static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

void config_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    copy_string(cfg->output_dir, sizeof(cfg->output_dir), "/userdata/photos");
    copy_string(cfg->input_name, sizeof(cfg->input_name), "gpio-keys");
    cfg->width = 2304;
    cfg->height = 1296;
    cfg->quality = 90;
    cfg->led = 1;
    cfg->led_count = 1;
    cfg->capture_timeout_ms = 3000;
    cfg->retries = 3;
    cfg->aiq = 1;
    cfg->fast_handoff = 1;
    cfg->log_level = 2;
}

static int parse_int(const char *value, int min, int max, int *out)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(value, &end, 10);
    if (errno != 0 || end == value)
        return -1;
    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return -1;
    if (v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

int config_parse_kv(app_config_t *cfg, const char *key, const char *value)
{
    if (!cfg || !key || !value)
        return -1;

    if (strcmp(key, "output_dir") == 0) {
        if (value[0] == '\0')
            return -1;
        copy_string(cfg->output_dir, sizeof(cfg->output_dir), value);
        return 0;
    }
    if (strcmp(key, "input_name") == 0) {
        if (value[0] == '\0')
            return -1;
        copy_string(cfg->input_name, sizeof(cfg->input_name), value);
        return 0;
    }
    if (strcmp(key, "width") == 0)
        return parse_int(value, 16, 8192, &cfg->width);
    if (strcmp(key, "height") == 0)
        return parse_int(value, 16, 8192, &cfg->height);
    if (strcmp(key, "quality") == 0)
        return parse_int(value, 1, 100, &cfg->quality);
    if (strcmp(key, "led") == 0)
        return parse_int(value, 0, 1, &cfg->led);
    if (strcmp(key, "led_count") == 0)
        return parse_int(value, 1, 16, &cfg->led_count);
    if (strcmp(key, "capture_timeout_ms") == 0)
        return parse_int(value, 100, 30000, &cfg->capture_timeout_ms);
    if (strcmp(key, "retries") == 0)
        return parse_int(value, 0, 10, &cfg->retries);
    if (strcmp(key, "aiq") == 0)
        return parse_int(value, 0, 1, &cfg->aiq);
    if (strcmp(key, "fast_handoff") == 0)
        return parse_int(value, 0, 1, &cfg->fast_handoff);
    if (strcmp(key, "log_level") == 0)
        return parse_int(value, 0, 3, &cfg->log_level);

    return -1;
}

int config_load_file(app_config_t *cfg, const char *path)
{
    FILE *fp;
    char line[512];
    int lineno = 0;
    int errors = 0;

    if (!cfg || !path)
        return -1;

    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        char *eq;
        char *key;
        char *value;

        lineno++;
        if (*p == '\0' || *p == '#')
            continue;

        eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "config: línea %d sin '=': %s\n", lineno, p);
            errors++;
            continue;
        }
        *eq = '\0';
        key = trim(p);
        value = trim(eq + 1);
        if (*key == '\0') {
            fprintf(stderr, "config: línea %d con clave vacía\n", lineno);
            errors++;
            continue;
        }
        if (config_parse_kv(cfg, key, value) != 0) {
            fprintf(stderr, "config: línea %d clave/valor inválido: %s=%s\n",
                    lineno, key, value);
            errors++;
        }
    }

    fclose(fp);
    return errors == 0 ? 0 : -1;
}

int config_validate(const app_config_t *cfg)
{
    if (!cfg)
        return -1;
    if (cfg->output_dir[0] == '\0')
        return -1;
    if (cfg->input_name[0] == '\0')
        return -1;
    if (cfg->width < 16 || cfg->height < 16)
        return -1;
    if (cfg->quality < 1 || cfg->quality > 100)
        return -1;
    if (cfg->led_count < 1 || cfg->led_count > 16)
        return -1;
    if (cfg->capture_timeout_ms < 100)
        return -1;
    return 0;
}
