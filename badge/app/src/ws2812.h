/*
 * File: app/src/ws2812.h
 * Purpose: WS2812 SPI and RGB encoding interface.
 */

#ifndef BADGE_WS2812_H
#define BADGE_WS2812_H

#include <stddef.h>
#include <stdint.h>

#define WS2812_BYTES_PER_LED 24

/* Encode RGB (r,g,b per LED) into WS2812 SPI bytes (GRB order). */
int ws2812_encode_leds(const uint8_t *rgb, int count, uint8_t *out, size_t outsz);

int ws2812_open(const char *dev);
int ws2812_write(int fd, const uint8_t *rgb, int count);
int ws2812_set(int fd, uint8_t r, uint8_t g, uint8_t b);
void ws2812_close(int fd);

#endif
