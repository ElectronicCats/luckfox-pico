/*
 * File: app/src/user_led.c
 * Purpose: User-owned RGB routine and button hooks for the badge.
 *
 * This is the file to edit when adding your own behavior. Keep it simple:
 * a routine fills one RGB value and returns immediately; the badge loop calls
 * it about every 20 ms. Never sleep, block, or access SPI directly here.
 *
 * Button mapping:
 *   USER1 / KEY_UP    -> user_led_on_button_up()
 *   USER2 / KEY_RIGHT -> user_led_on_button_right()
 *   USER3 / KEY_LEFT  -> next built-in/user routine
 *   USER4 / KEY_DOWN  -> previous built-in/user routine
 */

#include "user_led.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>

static void user_demo_frame(uint32_t elapsed_ms, uint8_t *rgb)
{
    uint32_t phase = elapsed_ms % 1800;
    uint8_t level;

    if (phase < 900)
        level = (uint8_t)(20 + phase * 180 / 900);
    else
        level = (uint8_t)(200 - (phase - 900) * 180 / 900);

    rgb[0] = level;
    rgb[1] = 0;
    rgb[2] = (uint8_t)(level / 2);
}

/*
 * Add more routines here. Each entry becomes available through USER3/USER4
 * and is appended after the built-in routines.
 */
const led_pattern_t user_led_patterns[] = {
    { "USER demo", user_demo_frame },
};

const size_t user_led_pattern_count =
    sizeof(user_led_patterns) / sizeof(user_led_patterns[0]);

void user_led_on_button_up(led_engine_t *engine)
{
    LOGI("user: USER1/KEY_UP\n");
    if (led_engine_select_user(engine, 0) == 0)
        LOGI("user: rutina '%s'\n", led_engine_pattern_name(engine));
}

void user_led_on_button_right(led_engine_t *engine)
{
    LOGI("user: USER2/KEY_RIGHT\n");
    led_engine_flash(engine, 0, 80, 80, 180);
}
