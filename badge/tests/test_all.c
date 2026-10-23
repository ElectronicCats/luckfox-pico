/*
 * File: tests/test_all.c
 * Purpose: Host unit tests for the pure badge_photo modules.
 */

#include "buttons.h"
#include "config.h"
#include "led_engine.h"
#include "meta.h"
#include "photo_path.h"
#include "ws2812.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_config(void)
{
    app_config_t cfg;
    char path[] = "/tmp/badge-config-XXXXXX";
    int fd;

    config_defaults(&cfg);
    CHECK(strcmp(cfg.output_dir, "/userdata/photos") == 0);
    CHECK(cfg.width == 2304 && cfg.height == 1296);
    CHECK(cfg.quality == 90);
    CHECK(config_validate(&cfg) == 0);

    CHECK(config_parse_kv(&cfg, "width", "1920") == 0 && cfg.width == 1920);
    CHECK(config_parse_kv(&cfg, "quality", "0") != 0);
    CHECK(config_parse_kv(&cfg, "unknown", "x") != 0);
    CHECK(config_parse_kv(&cfg, "output_dir", "") != 0);

    fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char *text = "# comment\n\n width = 640 \nquality=75\nled=0\n";
        CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
        close(fd);
        config_defaults(&cfg);
        CHECK(config_load_file(&cfg, path) == 0);
        CHECK(cfg.width == 640);
        CHECK(cfg.quality == 75);
        CHECK(cfg.led == 0);
        unlink(path);
    }
}

static void test_photo_path(void)
{
    char out[256];
    char tmp[1024];
    char dir[] = "/tmp/badge-photo-XXXXXX";
    char nested[512];
    char stage[512];
    char keep[1024];

    setenv("TZ", "UTC", 1);
    tzset();

    CHECK(photo_path_format("/tmp/x", 1700000000, 7, out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/tmp/x/IMG_20231114_221320_0007.jpg") == 0);
    CHECK(photo_path_format("/tmp/x", 0, 1, out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/tmp/x/IMG_boot_0001.jpg") == 0);
    CHECK(photo_path_make_temp("/tmp/x/a.jpg", "/tmp/stage",
                               tmp, sizeof(tmp)) == 0);
    CHECK(strcmp(tmp, "/tmp/stage/a.jpg.tmp") == 0);

    CHECK(mkdtemp(dir) != NULL);
    snprintf(nested, sizeof(nested), "%s/a/b", dir);
    CHECK(photo_path_ensure_dir(nested) == 0);
    CHECK(access(nested, F_OK) == 0);

    snprintf(stage, sizeof(stage), "%s/stage", dir);
    CHECK(photo_path_ensure_dir(stage) == 0);
    snprintf(tmp, sizeof(tmp), "%s/a.tmp", stage);
    snprintf(keep, sizeof(keep), "%s/keep.jpg", stage);
    {
        int fd = open(tmp, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0);
        if (fd >= 0)
            close(fd);
        fd = open(keep, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0);
        if (fd >= 0)
            close(fd);
    }
    CHECK(photo_path_clean_staging(stage) == 1);
    CHECK(access(tmp, F_OK) != 0);
    CHECK(access(keep, F_OK) == 0);
    unlink(keep);
    rmdir(stage);

    CHECK(photo_path_next(dir, out, sizeof(out)) == 0);
    CHECK(strstr(out, "/IMG_") != NULL);
    CHECK(access(out, F_OK) != 0);
    {
        int fd = open(out, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0);
        if (fd >= 0)
            close(fd);
    }
    {
        char second[256];
        CHECK(photo_path_next(dir, second, sizeof(second)) == 0);
        CHECK(strcmp(out, second) != 0);
    }
    unlink(out);
    rmdir(nested);
    snprintf(nested, sizeof(nested), "%s/a", dir);
    rmdir(nested);
    rmdir(dir);
}

static void test_ws2812(void)
{
    const uint8_t rgb[3] = { 0x12, 0x34, 0x56 };
    const uint8_t expected[24] = {
        0x08, 0x08, 0x0e, 0x0e, 0x08, 0x0e, 0x08, 0x08,
        0x08, 0x08, 0x08, 0x0e, 0x08, 0x08, 0x0e, 0x08,
        0x08, 0x0e, 0x08, 0x0e, 0x08, 0x0e, 0x0e, 0x08,
    };
    uint8_t out[24];

    CHECK(ws2812_encode_leds(rgb, 1, out, sizeof(out)) == 24);
    CHECK(memcmp(out, expected, sizeof(expected)) == 0);
    CHECK(ws2812_encode_leds(rgb, 1, out, 23) == -1);
    CHECK(ws2812_encode_leds(rgb, 0, out, sizeof(out)) == -1);
}

static void test_buttons(void)
{
    button_state_t state;
    struct input_event ev;

    buttons_state_init(&state);
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = KEY_ENTER;
    ev.value = 1;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_CAPTURE);
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_NONE);
    ev.value = 2;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_NONE);
    ev.value = 0;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_RELEASE);
    ev.value = 1;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_CAPTURE);
    ev.value = 0;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_RELEASE);

    ev.code = KEY_LEFT;
    ev.value = 1;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_LED_NEXT);
    ev.value = 0;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_RELEASE);

    ev.code = KEY_DOWN;
    ev.value = 1;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_LED_PREV);
    ev.value = 0;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_RELEASE);

    ev.code = KEY_UP;
    ev.value = 1;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_USER_UP);
    ev.value = 0;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_RELEASE);

    ev.code = KEY_RIGHT;
    ev.value = 1;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_USER_RIGHT);
    ev.value = 0;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_RELEASE);

    ev.type = EV_SYN;
    ev.code = KEY_ENTER;
    CHECK(buttons_process(&state, &ev) == BTN_ACTION_NONE);
}

static void test_led_engine(void)
{
    led_engine_t engine;
    int first;

    memset(&engine, 0, sizeof(engine));
    led_engine_init(&engine, -1, 1, 0);
    CHECK(led_engine_pattern_count(&engine) >= 7);
    first = led_engine_pattern_index(&engine);
    CHECK(led_engine_next(&engine) == 0);
    CHECK(led_engine_pattern_index(&engine) != first);
    CHECK(led_engine_prev(&engine) == 0);
    CHECK(led_engine_pattern_index(&engine) == first);
    CHECK(led_engine_select_user(&engine, 0) == 0);
    CHECK(led_engine_pattern_name(&engine) != NULL);
}

static void test_cmdline(void)
{
    const char *cmd =
        "loglevel=0 meta_load_addr=0x800000 rk_iqbin_size=0x29f10 "
        "rk_cam_fps=25 xmeta_load_addr=0x1";
    long v = 0;

    CHECK(cmdline_get_value(cmd, "meta_load_addr", &v) == 1 && v == 0x800000);
    CHECK(cmdline_get_value(cmd, "rk_iqbin_size", &v) == 1 && v == 0x29f10);
    CHECK(cmdline_get_value(cmd, "rk_cam_fps", &v) == 1 && v == 25);
    CHECK(cmdline_get_value(cmd, "meta_load_addr_extra", &v) == 0);
    CHECK(cmdline_get_value(cmd, "load_addr", &v) == 0);
}

int main(void)
{
    test_config();
    test_photo_path();
    test_ws2812();
    test_buttons();
    test_led_engine();
    test_cmdline();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("all host tests passed\n");
    return 0;
}
