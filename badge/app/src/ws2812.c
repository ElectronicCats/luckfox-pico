/*
 * File: app/src/ws2812.c
 * Purpose: SPI WS2812 driver and GRB encoder.
 */

#include "ws2812.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define WS2812_SPI_SPEED 2400000
#define WS2812_SPI_MODE 0
#define WS2812_SPI_BITS 8

static void encode_byte(uint8_t value, uint8_t *out)
{
    int bit;

    for (bit = 7; bit >= 0; bit--)
        *out++ = (value & (1u << bit)) ? 0x0e : 0x08;
}

int ws2812_encode_leds(const uint8_t *rgb, int count, uint8_t *out, size_t outsz)
{
    int led;
    size_t need;

    if (!rgb || !out || count < 1)
        return -1;
    need = (size_t)count * WS2812_BYTES_PER_LED;
    if (outsz < need)
        return -1;

    for (led = 0; led < count; led++) {
        uint8_t r = rgb[led * 3 + 0];
        uint8_t g = rgb[led * 3 + 1];
        uint8_t b = rgb[led * 3 + 2];

        /* WS2812 expects GRB order. */
        encode_byte(g, out);
        encode_byte(r, out + 8);
        encode_byte(b, out + 16);
        out += WS2812_BYTES_PER_LED;
    }
    return (int)need;
}

int ws2812_open(const char *dev)
{
    int fd;
    uint8_t mode = WS2812_SPI_MODE;
    uint8_t bits = WS2812_SPI_BITS;
    uint32_t speed = WS2812_SPI_SPEED;

    if (!dev)
        return -1;

    fd = open(dev, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int ws2812_write(int fd, const uint8_t *rgb, int count)
{
    uint8_t buf[WS2812_BYTES_PER_LED * 16];
    int len;

    if (fd < 0 || !rgb || count < 1 || count > 16)
        return -1;

    len = ws2812_encode_leds(rgb, count, buf, sizeof(buf));
    if (len < 0)
        return -1;

    if (write(fd, buf, (size_t)len) != len)
        return -1;
    return 0;
}

int ws2812_set(int fd, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rgb[3] = { r, g, b };
    return ws2812_write(fd, rgb, 1);
}

void ws2812_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
