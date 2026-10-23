/*
 * File: app/src/buttons.c
 * Purpose: gpio-keys discovery and debounced capture-button handling.
 */

#include "buttons.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EVDEV_MAX 32
#define EVDEV_NAME_MAX 256

void buttons_state_init(button_state_t *state)
{
    if (state)
        state->pressed = 0;
}

/* One bit per physical key; this keeps release edges independent even when
 * two buttons are held at once. */
static unsigned int button_bit(int code)
{
    switch (code) {
    case KEY_ENTER: return 1u << 0;
    case KEY_LEFT: return 1u << 1;
    case KEY_DOWN: return 1u << 2;
    case KEY_UP: return 1u << 3;
    case KEY_RIGHT: return 1u << 4;
    default: return 0;
    }
}

static button_action_t button_action(int code)
{
    switch (code) {
    case KEY_ENTER: return BTN_ACTION_CAPTURE;
    case KEY_LEFT: return BTN_ACTION_LED_NEXT;
    case KEY_DOWN: return BTN_ACTION_LED_PREV;
    case KEY_UP: return BTN_ACTION_USER_UP;
    case KEY_RIGHT: return BTN_ACTION_USER_RIGHT;
    default: return BTN_ACTION_NONE;
    }
}

button_action_t buttons_process(button_state_t *state,
                                const struct input_event *ev)
{
    unsigned int bit;

    if (!state || !ev || ev->type != EV_KEY)
        return BTN_ACTION_NONE;
    bit = button_bit(ev->code);
    if (!bit)
        return BTN_ACTION_NONE;

    if (ev->value == 1) {
        if (state->pressed & bit)
            return BTN_ACTION_NONE;
        state->pressed |= bit;
        return button_action(ev->code);
    }
    if (ev->value == 0) {
        state->pressed &= ~bit;
        return BTN_ACTION_RELEASE;
    }
    return BTN_ACTION_NONE;
}

static int try_open(const char *path, char *out, size_t outsz,
                    const char *preferred, int require_match)
{
    char name[EVDEV_NAME_MAX] = {0};
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0)
        return -1;

    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        name[0] = '\0';

    if (require_match) {
        int match = 0;
        if (preferred && preferred[0] && strstr(name, preferred))
            match = 1;
        if (!match && strstr(name, "gpio-keys"))
            match = 1;
        if (!match && strstr(name, "keys"))
            match = 1;
        if (!match) {
            close(fd);
            return -1;
        }
    }

    if (out && outsz)
        snprintf(out, outsz, "%s", path);
    return fd;
}

int buttons_open(const char *preferred, char *path, size_t pathsz)
{
    char dev[64];
    int i;

    /* If the caller passed a full path, try it first. */
    if (preferred && preferred[0] == '/') {
        int fd = try_open(preferred, path, pathsz, NULL, 0);
        if (fd >= 0)
            return fd;
    }

    for (i = 0; i < EVDEV_MAX; i++) {
        int fd;
        snprintf(dev, sizeof(dev), "/dev/input/event%d", i);
        fd = try_open(dev, path, pathsz, preferred, 1);
        if (fd >= 0)
            return fd;
    }

    /* Last-resort compatibility with the previous badge firmware. */
    return try_open("/dev/input/event1", path, pathsz, NULL, 0);
}

int buttons_read(int fd, struct input_event *ev)
{
    ssize_t n;

    if (fd < 0 || !ev)
        return -1;
    n = read(fd, ev, sizeof(*ev));
    if (n == (ssize_t)sizeof(*ev))
        return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    return -1;
}

void buttons_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
