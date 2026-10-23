/*
 * File: app/src/user_led.h
 * Purpose: Hooks for the two user-programmable badge buttons.
 */

#ifndef BADGE_USER_LED_H
#define BADGE_USER_LED_H

#include "led_engine.h"

/* USER1 (KEY_UP): edit this function for your own action. */
void user_led_on_button_up(led_engine_t *engine);

/* USER2 (KEY_RIGHT): edit this function for your own action. */
void user_led_on_button_right(led_engine_t *engine);

#endif
