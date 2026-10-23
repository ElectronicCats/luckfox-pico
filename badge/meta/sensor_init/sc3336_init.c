/*
 * Badge Hack GDL 2026 — SC3336 sensor init for the fastboot MCU.
 *
 * The badge has no IR-cut, no IR LED and no white LED. All unused GPIOs are
 * set to GPIO_INVALID so the MCU never drives the photo button (GPIO1_D3) or
 * any other badge signal.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sensor_define.h"
#include "sensor_init_info.h"

int main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("usage: sc3336_init <output file>\n");
		return -1;
	}

	const char *file_path = argv[1];
	int fd = open(file_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) {
		printf("create %s failed: %s\n", file_path, strerror(errno));
		return -1;
	}

	struct sensor_init_cfg init_data = {
		.head                         = 0,
		.len                          = 0,
		.crc32                        = 0,
		.cam_w                        = 2304,
		.cam_h                        = 1296,
		.ircut_a = {
			.gpio_index = GPIO_INVALID,
			.gpio_func = 0,
		},
		.ircut_b = {
			.gpio_index = GPIO_INVALID,
			.gpio_func = 0,
		},
		.led_ir_enable = {
			.gpio_index = GPIO_INVALID,
			.gpio_func = 0,
		},
		.led_ir = {
			.gpio_index = GPIO_INVALID,
			.gpio_func = 0,
			.pwm_channel = 0,
			.pwm_period = 0,
			.pwm_pulse = 0,
		},
		.led_white_enable = {
			.gpio_index = GPIO_INVALID,
			.gpio_func = 0,
		},
		.led_white = {
			.gpio_index = GPIO_INVALID,
			.gpio_func = 0,
			.pwm_channel = 0,
			.pwm_period = 0,
			.pwm_pulse = 0,
		},
		.gpio_adc_enable = {
			.gpio_index = GPIO_INVALID,
			.gpio_func = 0,
		},
		.gpio_adc = {
			.gpio_index = GPIO_INVALID,
			.gpio_func = 0,
			.adc_channel = 0,
			.adc_direction = 0,
		},
		.als_type                     = ALS_TYPE_NONE,
		.als_value                    = 0,
	};

	ssize_t len = write(fd, &init_data, sizeof(init_data));
	int saved_errno = errno;
	close(fd);

	if (len != (ssize_t)sizeof(init_data)) {
		printf("write error: %s\n", strerror(saved_errno));
		return -1;
	}

	printf("make sensor init setting file [%s] len=%zd\n", file_path, len);
	return 0;
}
