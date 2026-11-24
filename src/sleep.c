/*
 * SPDX-License-Identifier: MIT
 *
 * "sleep" shell command: power the SoC off (System OFF) until the reset
 * button is pressed, or until the IMU detects motion. Waking is a reset, so
 * the firmware starts from the beginning.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/poweroff.h>

#include "app.h"

#define WAKE_DEFAULT_MG 125
#define WAKE_MIN_MG     32
#define WAKE_MAX_MG     1968

static const struct gpio_dt_spec imu_int1 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(lsm6ds3tr_c), irq_gpios);

static void power_off(void)
{
	/* Let the message reach the terminal before the link drops. */
	k_msleep(500);
	blink_stop();
	sys_poweroff();
}

static int cmd_sleep_off(const struct shell *sh, size_t argc, char **argv)
{
	int err = imu_power_down();

	if (err) {
		shell_warn(sh, "Could not power down the IMU (err %d)", err);
	}

	/* Make sure a previous motion sleep cannot wake this one. */
	gpio_pin_interrupt_configure_dt(&imu_int1, GPIO_INT_DISABLE);
	gpio_pin_configure_dt(&imu_int1, GPIO_DISCONNECTED);

	shell_print(sh, "Sleeping until the reset button is pressed...");
	power_off();

	return 0;
}

static int cmd_sleep_motion(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t threshold_mg = WAKE_DEFAULT_MG;
	int err = 0;

	if (argc > 1) {
		threshold_mg = shell_strtoul(argv[1], 10, &err);
		if (err || threshold_mg < WAKE_MIN_MG || threshold_mg > WAKE_MAX_MG) {
			shell_error(sh, "Threshold must be %d to %d mg", WAKE_MIN_MG, WAKE_MAX_MG);
			return -EINVAL;
		}
	}

	if (!gpio_is_ready_dt(&imu_int1)) {
		shell_error(sh, "IMU interrupt pin not ready");
		return -ENODEV;
	}

	err = imu_arm_wake(&threshold_mg);
	if (err) {
		shell_error(sh, "Could not arm the IMU (err %d)", err);
		return err;
	}

	/* A high level on the IMU's INT1 pin wakes the SoC from System OFF. */
	err = gpio_pin_configure_dt(&imu_int1, GPIO_INPUT);
	if (!err) {
		err = gpio_pin_interrupt_configure_dt(&imu_int1, GPIO_INT_LEVEL_ACTIVE);
	}
	if (err) {
		shell_error(sh, "Could not arm the wake-up pin (err %d)", err);
		return err;
	}

	shell_print(sh, "Sleeping until motion above %u mg, or the reset button...",
		    threshold_mg);
	power_off();

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sleep_cmds,
	SHELL_CMD(off, NULL, "Deep sleep until the reset button is pressed", cmd_sleep_off),
	SHELL_CMD_ARG(motion, NULL,
		      "[mg]  Sleep until the board is moved (default 125 mg) or reset",
		      cmd_sleep_motion, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sleep, &sleep_cmds, "Power down until reset or motion", NULL);
