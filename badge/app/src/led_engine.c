/*
 * File: app/src/led_engine.c
 * Purpose: Non-blocking RGB patterns for the badge NeoPixel.
 *
 * The engine is intentionally small and integer-only so it can run from the
 * existing 20 ms badge_photo loop without threads, malloc, or blocking sleeps.
 * User routines are appended from user_led.c.
 */

#include "led_engine.h"
#include "log.h"
#include "ws2812.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static void hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *rgb)
{
    uint8_t region;
    uint8_t remainder;
    uint8_t p;
    uint8_t q;
    uint8_t t;
    unsigned int s_rem;
    unsigned int s_inv;

    if (!rgb)
        return;
    if (s == 0) {
        rgb[0] = rgb[1] = rgb[2] = v;
        return;
    }
    region = (uint8_t)(h / 43);
    remainder = (uint8_t)((h - region * 43) * 6);
    s_rem = (unsigned int)s * remainder / 255;
    s_inv = (unsigned int)s * (255 - remainder) / 255;
    p = (uint8_t)((unsigned int)v * (255 - s) / 255);
    q = (uint8_t)((unsigned int)v * (255 - s_rem) / 255);
    t = (uint8_t)((unsigned int)v * (255 - s_inv) / 255);

    switch (region) {
    case 0: rgb[0] = v; rgb[1] = t; rgb[2] = p; break;
    case 1: rgb[0] = q; rgb[1] = v; rgb[2] = p; break;
    case 2: rgb[0] = p; rgb[1] = v; rgb[2] = t; break;
    case 3: rgb[0] = p; rgb[1] = q; rgb[2] = v; break;
    case 4: rgb[0] = t; rgb[1] = p; rgb[2] = v; break;
    default: rgb[0] = v; rgb[1] = p; rgb[2] = q; break;
    }
}

static void pattern_off(uint32_t elapsed_ms, uint8_t *rgb)
{
    (void)elapsed_ms;
    rgb[0] = rgb[1] = rgb[2] = 0;
}

static void pattern_breath(uint32_t elapsed_ms, uint8_t *rgb)
{
    uint32_t phase = elapsed_ms % 2400;
    uint32_t up = phase < 1200 ? phase : 2400 - phase;
    uint8_t value = (uint8_t)(12 + up * 180 / 1200);

    rgb[0] = (uint8_t)(value * 3 / 4);
    rgb[1] = (uint8_t)(value / 5);
    rgb[2] = value;
}

static void pattern_rainbow(uint32_t elapsed_ms, uint8_t *rgb)
{
    hsv_to_rgb((uint8_t)(elapsed_ms / 24), 255, 170, rgb);
}

static void pattern_pulse(uint32_t elapsed_ms, uint8_t *rgb)
{
    uint32_t phase = elapsed_ms % 2000;
    uint8_t value;

    if (phase < 120)
        value = 210;
    else if (phase < 600)
        value = (uint8_t)(180 - (phase - 120) * 150 / 480);
    else
        value = (uint8_t)(30 + (phase - 600) * 80 / 1400);

    rgb[0] = 0;
    rgb[1] = (uint8_t)(value / 2);
    rgb[2] = value;
}

static void pattern_alert(uint32_t elapsed_ms, uint8_t *rgb)
{
    if ((elapsed_ms / 250) & 1) {
        rgb[0] = 0;
        rgb[1] = 0;
        rgb[2] = 90;
    } else {
        rgb[0] = 90;
        rgb[1] = 0;
        rgb[2] = 0;
    }
}

static void pattern_candle(uint32_t elapsed_ms, uint8_t *rgb)
{
    uint32_t n = (elapsed_ms / 45) * 1103515245u + 12345u;
    uint8_t value;

    n ^= n >> 13;
    n ^= n << 7;
    value = (uint8_t)(70 + (n >> 16) % 120);
    rgb[0] = value;
    rgb[1] = (uint8_t)(value / 3);
    rgb[2] = 0;
}

static const led_pattern_t builtin_patterns[] = {
    { "Off", pattern_off },
    { "Breath", pattern_breath },
    { "Rainbow", pattern_rainbow },
    { "Pulse", pattern_pulse },
    { "Alert", pattern_alert },
    { "Candle", pattern_candle },
};

static int builtin_count(void)
{
    return (int)ARRAY_SIZE(builtin_patterns);
}

static int total_pattern_count(void)
{
    return builtin_count() + (int)user_led_pattern_count;
}

const char *led_engine_pattern_at(int index)
{
    const led_pattern_t *pattern = NULL;

    if (index < 0)
        return NULL;
    if (index < builtin_count()) {
        pattern = &builtin_patterns[index];
    } else {
        index -= builtin_count();
        if (index >= 0 && index < (int)user_led_pattern_count)
            pattern = &user_led_patterns[index];
    }
    return pattern && pattern->name ? pattern->name : NULL;
}

static void write_color(led_engine_t *engine, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rgb[LED_ENGINE_MAX_LEDS * 3];
    int count;
    int i;

    if (!engine || engine->fd < 0 || engine->led_count < 1)
        return;
    count = engine->led_count;
    if (count > LED_ENGINE_MAX_LEDS)
        count = LED_ENGINE_MAX_LEDS;
    for (i = 0; i < count; i++) {
        rgb[i * 3 + 0] = r;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = b;
    }
    (void)ws2812_write(engine->fd, rgb, count);
}

void led_engine_init(led_engine_t *engine, int fd, int led_count, uint32_t now_ms)
{
    if (!engine)
        return;
    memset(engine, 0, sizeof(*engine));
    engine->fd = fd;
    engine->led_count = led_count;
    engine->pattern_count = total_pattern_count();
    engine->pattern = 0; /* Start off; USER3/USER4 select a routine. */
    engine->now_ms = now_ms;
    engine->pattern_started_ms = now_ms;
    engine->last_render_ms = now_ms - LED_ENGINE_RENDER_MS;
    LOGI("led: %d rutinas, inicial '%s'\n", engine->pattern_count,
         led_engine_pattern_name(engine));
}

void led_engine_tick(led_engine_t *engine, uint32_t now_ms)
{
    const led_pattern_t *pattern;
    uint8_t rgb[3] = { 0, 0, 0 };

    if (!engine)
        return;
    engine->now_ms = now_ms;

    if (engine->flash_active &&
        (int32_t)(now_ms - engine->flash_until_ms) >= 0)
        engine->flash_active = 0;

    if (engine->fd < 0 || engine->led_count < 1)
        return;
    if ((uint32_t)(now_ms - engine->last_render_ms) < LED_ENGINE_RENDER_MS)
        return;
    engine->last_render_ms = now_ms;

    if (engine->flash_active) {
        write_color(engine, engine->flash_rgb[0], engine->flash_rgb[1],
                    engine->flash_rgb[2]);
        return;
    }

    if (engine->pattern < 0 || engine->pattern >= engine->pattern_count)
        engine->pattern = 0;
    pattern = NULL;
    if (engine->pattern < builtin_count())
        pattern = &builtin_patterns[engine->pattern];
    else {
        int user = engine->pattern - builtin_count();
        if (user >= 0 && user < (int)user_led_pattern_count)
            pattern = &user_led_patterns[user];
    }
    if (pattern && pattern->frame)
        pattern->frame((uint32_t)(now_ms - engine->pattern_started_ms), rgb);
    write_color(engine, rgb[0], rgb[1], rgb[2]);
}

int led_engine_select(led_engine_t *engine, int index)
{
    if (!engine || index < 0 || index >= engine->pattern_count)
        return -1;
    engine->pattern = index;
    engine->pattern_started_ms = engine->now_ms;
    engine->last_render_ms = engine->now_ms - LED_ENGINE_RENDER_MS;
    return 0;
}

int led_engine_select_user(led_engine_t *engine, int user_index)
{
    if (!engine || user_index < 0 || user_index >= (int)user_led_pattern_count)
        return -1;
    return led_engine_select(engine, builtin_count() + user_index);
}

int led_engine_next(led_engine_t *engine)
{
    if (!engine || engine->pattern_count < 1)
        return -1;
    return led_engine_select(engine, (engine->pattern + 1) % engine->pattern_count);
}

int led_engine_prev(led_engine_t *engine)
{
    if (!engine || engine->pattern_count < 1)
        return -1;
    return led_engine_select(engine,
                             (engine->pattern - 1 + engine->pattern_count) %
                                 engine->pattern_count);
}

void led_engine_flash(led_engine_t *engine, uint8_t r, uint8_t g, uint8_t b,
                      uint32_t duration_ms)
{
    if (!engine)
        return;
    engine->flash_rgb[0] = r;
    engine->flash_rgb[1] = g;
    engine->flash_rgb[2] = b;
    if (duration_ms < LED_ENGINE_RENDER_MS)
        duration_ms = LED_ENGINE_RENDER_MS;
    engine->flash_until_ms = engine->now_ms + duration_ms;
    engine->flash_active = 1;
}

int led_engine_pattern_count(const led_engine_t *engine)
{
    return engine ? engine->pattern_count : 0;
}

int led_engine_pattern_index(const led_engine_t *engine)
{
    return engine ? engine->pattern : -1;
}

const char *led_engine_pattern_name(const led_engine_t *engine)
{
    const char *name = NULL;

    if (engine)
        name = led_engine_pattern_at(engine->pattern);
    return name ? name : "?";
}
