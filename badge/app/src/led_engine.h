/*
 * File: app/src/led_engine.h
 * Purpose: Non-blocking RGB pattern engine and user extension interface.
 */

#ifndef BADGE_LED_ENGINE_H
#define BADGE_LED_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#define LED_ENGINE_MAX_LEDS 16
#define LED_ENGINE_RENDER_MS 20

typedef void (*led_frame_fn)(uint32_t elapsed_ms, uint8_t *rgb);

typedef struct led_pattern {
    const char *name;
    led_frame_fn frame;
} led_pattern_t;

typedef struct led_engine {
    int fd;
    int led_count;
    int pattern;
    int pattern_count;
    uint32_t now_ms;
    uint32_t pattern_started_ms;
    uint32_t last_render_ms;
    uint32_t flash_until_ms;
    uint8_t flash_rgb[3];
    int flash_active;
} led_engine_t;

/* Extensions supplied by user_led.c; see the badge_hackgdl_2026 documentation. */
extern const led_pattern_t user_led_patterns[];
extern const size_t user_led_pattern_count;

void led_engine_init(led_engine_t *engine, int fd, int led_count, uint32_t now_ms);
void led_engine_tick(led_engine_t *engine, uint32_t now_ms);
int led_engine_next(led_engine_t *engine);
int led_engine_prev(led_engine_t *engine);
int led_engine_select(led_engine_t *engine, int index);
int led_engine_select_user(led_engine_t *engine, int user_index);
void led_engine_flash(led_engine_t *engine, uint8_t r, uint8_t g, uint8_t b,
                      uint32_t duration_ms);
int led_engine_pattern_count(const led_engine_t *engine);
int led_engine_pattern_index(const led_engine_t *engine);
const char *led_engine_pattern_name(const led_engine_t *engine);
const char *led_engine_pattern_at(int index);

#endif
