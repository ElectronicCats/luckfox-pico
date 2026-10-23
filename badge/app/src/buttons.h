/*
 * File: app/src/buttons.h
 * Purpose: Button event types and state interface.
 */

#ifndef BADGE_BUTTONS_H
#define BADGE_BUTTONS_H

#include <linux/input.h>
#include <stddef.h>

typedef enum button_action {
    BTN_ACTION_NONE = 0,
    BTN_ACTION_CAPTURE,       /* KEY_ENTER: top capture button */
    BTN_ACTION_LED_NEXT,     /* KEY_LEFT: USER3 */
    BTN_ACTION_LED_PREV,     /* KEY_DOWN: USER4 */
    BTN_ACTION_USER_UP,      /* KEY_UP: USER1, free for user code */
    BTN_ACTION_USER_RIGHT,   /* KEY_RIGHT: USER2, free for user code */
    BTN_ACTION_RELEASE,
} button_action_t;

typedef struct button_state {
    unsigned int pressed;
} button_state_t;

void buttons_state_init(button_state_t *state);
button_action_t buttons_process(button_state_t *state,
                                const struct input_event *ev);
int buttons_open(const char *preferred, char *path, size_t pathsz);
int buttons_read(int fd, struct input_event *ev);
void buttons_close(int fd);

#endif
