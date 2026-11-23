/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(lsm6ds3tr_c));

static void log_accel(void)
{
	struct sensor_value accel[3];

	if (sensor_sample_fetch(imu) < 0 ||
	    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, accel) < 0) {
		LOG_WRN("IMU read failed");
		return;
	}

	LOG_INF("accel x=%.2f y=%.2f z=%.2f m/s^2",
		sensor_value_to_double(&accel[0]),
		sensor_value_to_double(&accel[1]),
		sensor_value_to_double(&accel[2]));
}

int main(void)
{
	bool imu_ok = device_is_ready(imu);

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED device not ready");
		return 0;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	if (imu_ok) {
		struct sensor_value odr = { .val1 = 104 };

		sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
				SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	} else {
		LOG_WRN("IMU not ready");
	}

	while (1) {
		gpio_pin_toggle_dt(&led);
		if (imu_ok) {
			log_accel();
		}
		k_msleep(500);
	}

	return 0;
}
