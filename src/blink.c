/*
 * SPDX-License-Identifier: MIT
 *
 * RGB LED blinker with a shell command to change its rate and pattern.
 * Both values are kept in the settings storage partition across resets.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include "app.h"

LOG_MODULE_REGISTER(blink, LOG_LEVEL_INF);

#define RATE_MIN_MS     20
#define RATE_MAX_MS     60000
#define RATE_DEFAULT_MS 1000

#define PATTERN_MAX_LEN 32
#define PATTERN_DEFAULT "rgb"
#define PATTERN_HELP \
	"r g b = red green blue, y c m = yellow cyan magenta, w = white, - = off"

#define RED   BIT(0)
#define GREEN BIT(1)
#define BLUE  BIT(2)

/* Indexed by the bits above. */
static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
};

static K_MUTEX_DEFINE(lock);
static K_SEM_DEFINE(changed, 0, 1);
static uint32_t rate_ms = RATE_DEFAULT_MS;
static char pattern[PATTERN_MAX_LEN + 1] = PATTERN_DEFAULT;

/* LEDs lit for one pattern character, or -1 if the character is not valid. */
static int step_leds(char c)
{
	switch (c) {
	case 'r': return RED;
	case 'g': return GREEN;
	case 'b': return BLUE;
	case 'y': return RED | GREEN;
	case 'c': return GREEN | BLUE;
	case 'm': return RED | BLUE;
	case 'w': return RED | GREEN | BLUE;
	case '-': return 0;
	default:  return -1;
	}
}

static bool rate_valid(uint32_t ms)
{
	return ms >= RATE_MIN_MS && ms <= RATE_MAX_MS;
}

static bool pattern_valid(const char *p)
{
	size_t len = strlen(p);

	if (len == 0 || len > PATTERN_MAX_LEN) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (step_leds(p[i]) < 0) {
			return false;
		}
	}
	return true;
}

static void apply_rate(uint32_t ms)
{
	k_mutex_lock(&lock, K_FOREVER);
	rate_ms = ms;
	k_mutex_unlock(&lock);
	k_sem_give(&changed);
}

static void apply_pattern(const char *p)
{
	k_mutex_lock(&lock, K_FOREVER);
	strcpy(pattern, p);
	k_mutex_unlock(&lock);
	k_sem_give(&changed);
}

/* Called when main() loads settings, for each stored "blink/..." value. */
static int blink_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			      void *cb_arg)
{
	ssize_t rc;

	if (settings_name_steq(name, "rate", NULL)) {
		uint32_t ms;

		if (len != sizeof(ms)) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, &ms, sizeof(ms));
		if (rc < 0) {
			return rc;
		}
		if (rate_valid(ms)) {
			apply_rate(ms);
		}
		return 0;
	}

	if (settings_name_steq(name, "pattern", NULL)) {
		char buf[PATTERN_MAX_LEN + 1] = { 0 };

		if (len > PATTERN_MAX_LEN) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, buf, len);
		if (rc < 0) {
			return rc;
		}
		if (pattern_valid(buf)) {
			apply_pattern(buf);
		}
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(blink, "blink", NULL, blink_settings_set, NULL, NULL);

static void blink_thread(void *p1, void *p2, void *p3)
{
	size_t step = 0;

	ARRAY_FOR_EACH(leds, i) {
		if (gpio_is_ready_dt(&leds[i])) {
			gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		}
	}

	while (1) {
		uint32_t ms;
		int lit;

		k_mutex_lock(&lock, K_FOREVER);
		if (step >= strlen(pattern)) {
			step = 0;
		}
		lit = step_leds(pattern[step]);
		ms = rate_ms;
		k_mutex_unlock(&lock);

		ARRAY_FOR_EACH(leds, i) {
			gpio_pin_set_dt(&leds[i], (lit & BIT(i)) != 0);
		}

		step++;
		if (k_sem_take(&changed, K_MSEC(ms)) == 0) {
			/* New rate or pattern: start over. */
			step = 0;
		}
	}
}

K_THREAD_DEFINE(blink_tid, 2048, blink_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

void blink_stop(void)
{
	k_thread_suspend(blink_tid);

	ARRAY_FOR_EACH(leds, i) {
		gpio_pin_set_dt(&leds[i], 0);
	}
}

static int cmd_blink(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lock, K_FOREVER);
	shell_print(sh, "rate %u ms, pattern %s", rate_ms, pattern);
	k_mutex_unlock(&lock);

	return 0;
}

static int cmd_blink_rate(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	unsigned long ms = shell_strtoul(argv[1], 10, &err);
	uint32_t value;

	if (err || !rate_valid(ms)) {
		shell_error(sh, "Rate must be %d to %d ms", RATE_MIN_MS, RATE_MAX_MS);
		return -EINVAL;
	}

	value = ms;
	apply_rate(value);

	err = settings_save_one("blink/rate", &value, sizeof(value));
	if (err) {
		shell_error(sh, "Applied, but not saved (err %d)", err);
		return err;
	}

	return 0;
}

static int cmd_blink_pattern(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	if (!pattern_valid(argv[1])) {
		shell_error(sh, "Pattern must be 1 to %d of: %s", PATTERN_MAX_LEN, PATTERN_HELP);
		return -EINVAL;
	}

	apply_pattern(argv[1]);

	err = settings_save_one("blink/pattern", argv[1], strlen(argv[1]));
	if (err) {
		shell_error(sh, "Applied, but not saved (err %d)", err);
		return err;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(blink_cmds,
	SHELL_CMD_ARG(rate, NULL, "<ms>  Time per pattern step", cmd_blink_rate, 2, 0),
	SHELL_CMD_ARG(pattern, NULL, "<steps>  One character per step: " PATTERN_HELP,
		      cmd_blink_pattern, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(blink, &blink_cmds, "Show or change the LED blink rate and pattern",
		   cmd_blink);
