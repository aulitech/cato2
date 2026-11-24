/*
 * SPDX-License-Identifier: MIT
 *
 * LSM6DS3TR-C accelerometer/gyroscope, driven directly over I2C so that every
 * register is reachable from the shell.
 *
 * The chip's registers are the only copy of the configuration: status and
 * readings are decoded from what the chip currently holds, so raw register
 * writes and the named commands can be mixed freely. "imu save" stores the
 * configuration registers in settings; they are applied again at boot.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

#include "app.h"

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

#define IMU_NODE DT_NODELABEL(lsm6ds3tr_c)

#define REG_WHO_AM_I    0x0f
#define REG_CTRL1_XL    0x10
#define REG_CTRL2_G     0x11
#define REG_CTRL3_C     0x12
#define REG_CTRL6_C     0x15
#define REG_CTRL7_G     0x16
#define REG_WAKE_UP_SRC 0x1b
#define REG_OUT_TEMP_L  0x20
#define REG_OUTX_L_G    0x22
#define REG_OUTX_L_XL   0x28
#define REG_TAP_CFG     0x58
#define REG_WAKE_UP_THS 0x5b
#define REG_WAKE_UP_DUR 0x5c
#define REG_MD1_CFG     0x5e
#define REG_LAST        0x7f

#define WHO_AM_I_VALUE 0x6a

#define CTRL_ODR_SHIFT 4
#define CTRL_FS_SHIFT  2
#define CTRL_FS_MASK   (0x3 << CTRL_FS_SHIFT)
#define CTRL2_FS_125   BIT(1)

#define CTRL3_BOOT     BIT(7)
#define CTRL3_BDU      BIT(6)
#define CTRL3_IF_INC   BIT(2)
#define CTRL3_SW_RESET BIT(0)

#define CTRL6_XL_LOW_POWER BIT(4)
#define CTRL7_G_LOW_POWER  BIT(7)

#define TAP_CFG_INT_EN    BIT(7)
#define TAP_CFG_LATCH     BIT(0)
#define MD1_CFG_INT1_WU   BIT(5)

/* Used when nothing has been saved: both sensors at 104 Hz, +-2 g, +-250 dps. */
#define DEFAULT_CTRL1_XL (4 << CTRL_ODR_SHIFT)
#define DEFAULT_CTRL2_G  (4 << CTRL_ODR_SHIFT)
#define DEFAULT_CTRL3_C  (CTRL3_BDU | CTRL3_IF_INC)

/* Motion wake-up: 26 Hz low-power accelerometer, +-2 g. */
#define WAKE_CTRL1_XL   (2 << CTRL_ODR_SHIFT)
#define WAKE_MG_PER_LSB 31.25
#define WAKE_THS_MAX    63

#define STANDARD_GRAVITY 9.80665

enum sensor { ACCEL, GYRO };

static const struct i2c_dt_spec bus = I2C_DT_SPEC_GET(IMU_NODE);

/* Output data rates, indexed by the ODR field of CTRL1_XL / CTRL2_G. */
static const char *const odr_names[] = {
	"0", "12.5", "26", "52", "104", "208", "416", "833", "1660", "3330", "6660",
};

/* Full-scale ranges and sensitivities, indexed by the FS field. */
static const uint8_t accel_range_g[] = { 2, 16, 4, 8 };
static const double accel_mg_per_lsb[] = { 0.061, 0.488, 0.122, 0.244 };
static const uint16_t gyro_range_dps[] = { 250, 500, 1000, 2000 };
static const double gyro_mdps_per_lsb[] = { 8.75, 17.5, 35.0, 70.0 };
#define GYRO_125_MDPS_PER_LSB 4.375

/*
 * Configuration registers kept by "imu save": FIFO control, interrupt pin
 * routing, CTRL1_XL to MASTER_CONFIG, and the tap/wake-up/free-fall block.
 */
static const struct {
	uint8_t start;
	uint8_t count;
} saved_ranges[] = {
	{ 0x06, 5 },
	{ 0x0d, 2 },
	{ 0x10, 11 },
	{ 0x58, 8 },
};

#define SAVED_SIZE (5 + 2 + 11 + 8)

static uint8_t saved_config[SAVED_SIZE];
static bool have_saved_config;
static bool ready;

static int reg_read(uint8_t reg, uint8_t *value)
{
	return i2c_reg_read_byte_dt(&bus, reg, value);
}

static int reg_write(uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(&bus, reg, value);
}

static int reg_update(uint8_t reg, uint8_t mask, uint8_t value)
{
	return i2c_reg_update_byte_dt(&bus, reg, mask, value);
}

static int apply_saved_config(void)
{
	const uint8_t *p = saved_config;
	int err;

	ARRAY_FOR_EACH(saved_ranges, i) {
		for (uint8_t n = 0; n < saved_ranges[i].count; n++, p++) {
			uint8_t reg = saved_ranges[i].start + n;
			uint8_t value = *p;

			if (reg == REG_CTRL3_C) {
				value &= ~(CTRL3_BOOT | CTRL3_SW_RESET);
			}
			err = reg_write(reg, value);
			if (err) {
				return err;
			}
		}
	}

	return 0;
}

static int apply_default_config(void)
{
	int err = reg_write(REG_CTRL3_C, DEFAULT_CTRL3_C);

	if (!err) {
		err = reg_write(REG_CTRL1_XL, DEFAULT_CTRL1_XL);
	}
	if (!err) {
		err = reg_write(REG_CTRL2_G, DEFAULT_CTRL2_G);
	}
	return err;
}

/* Back to the chip's power-on register values. */
static int software_reset(void)
{
	uint8_t ctrl3 = CTRL3_SW_RESET;
	int err = reg_write(REG_CTRL3_C, CTRL3_SW_RESET);

	for (int i = 0; !err && (ctrl3 & CTRL3_SW_RESET) && i < 10; i++) {
		k_msleep(1);
		err = reg_read(REG_CTRL3_C, &ctrl3);
	}
	if (!err && (ctrl3 & CTRL3_SW_RESET)) {
		err = -ETIMEDOUT;
	}
	return err;
}

int imu_init(void)
{
	uint8_t id = 0;
	int err;

	if (!i2c_is_ready_dt(&bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	err = reg_read(REG_WHO_AM_I, &id);
	if (err || id != WHO_AM_I_VALUE) {
		LOG_ERR("IMU not found (err %d, id 0x%02x)", err, id);
		return err ? err : -ENODEV;
	}

	/* It may still hold a motion wake-up or other earlier configuration. */
	err = software_reset();
	if (!err) {
		err = have_saved_config ? apply_saved_config() : apply_default_config();
	}
	if (err) {
		LOG_ERR("IMU configuration failed (err %d)", err);
		return err;
	}

	ready = true;
	LOG_INF("IMU ready, %s configuration", have_saved_config ? "saved" : "default");

	return 0;
}

int imu_power_down(void)
{
	int err;

	if (!ready) {
		return -ENODEV;
	}

	err = reg_write(REG_CTRL1_XL, 0);
	if (!err) {
		err = reg_write(REG_CTRL2_G, 0);
	}
	return err;
}

int imu_arm_wake(uint32_t *threshold_mg)
{
	uint32_t ths = (uint32_t)(*threshold_mg / WAKE_MG_PER_LSB + 0.5);
	uint8_t source;
	int err;

	if (!ready) {
		return -ENODEV;
	}

	ths = CLAMP(ths, 1, WAKE_THS_MAX);
	*threshold_mg = (uint32_t)(ths * WAKE_MG_PER_LSB + 0.5);

	/* Start from a known state so that no other interrupt source is routed. */
	err = software_reset();
	if (!err) {
		err = reg_write(REG_CTRL3_C, DEFAULT_CTRL3_C);
	}
	if (!err) {
		err = reg_write(REG_CTRL6_C, CTRL6_XL_LOW_POWER);
	}
	if (!err) {
		err = reg_write(REG_CTRL1_XL, WAKE_CTRL1_XL);
	}
	if (!err) {
		err = reg_write(REG_WAKE_UP_DUR, 0);
	}
	if (!err) {
		err = reg_write(REG_WAKE_UP_THS, ths);
	}
	if (!err) {
		err = reg_write(REG_TAP_CFG, TAP_CFG_INT_EN | TAP_CFG_LATCH);
	}
	if (!err) {
		err = reg_write(REG_MD1_CFG, MD1_CFG_INT1_WU);
	}
	if (err) {
		return err;
	}

	/* Let the filter settle, then discard anything latched while it did. */
	k_msleep(300);
	return reg_read(REG_WAKE_UP_SRC, &source);
}

/* ------------------------------------------------------------ settings --- */

static int imu_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			    void *cb_arg)
{
	ssize_t rc;

	if (!settings_name_steq(name, "config", NULL)) {
		return -ENOENT;
	}
	if (len != sizeof(saved_config)) {
		/* Saved by a firmware with a different register list: ignore. */
		return 0;
	}

	rc = read_cb(cb_arg, saved_config, sizeof(saved_config));
	if (rc < 0) {
		return rc;
	}
	have_saved_config = true;

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(imu, "imu", NULL, imu_settings_set, NULL, NULL);

/* --------------------------------------------------------------- shell --- */

static bool check_ready(const struct shell *sh)
{
	if (!ready) {
		shell_error(sh, "IMU not ready");
	}
	return ready;
}

static int report(const struct shell *sh, int err)
{
	if (err) {
		shell_error(sh, "IMU access failed (err %d)", err);
	}
	return err;
}

static double gyro_sensitivity(uint8_t ctrl2)
{
	if (ctrl2 & CTRL2_FS_125) {
		return GYRO_125_MDPS_PER_LSB;
	}
	return gyro_mdps_per_lsb[(ctrl2 & CTRL_FS_MASK) >> CTRL_FS_SHIFT];
}

static int read_axes(uint8_t reg, int16_t axes[3])
{
	uint8_t raw[6];
	int err = i2c_burst_read_dt(&bus, reg, raw, sizeof(raw));

	for (int i = 0; !err && i < 3; i++) {
		axes[i] = (int16_t)sys_get_le16(&raw[2 * i]);
	}
	return err;
}

static int print_accel(const struct shell *sh)
{
	uint8_t ctrl1;
	int16_t axes[3];
	double scale;
	int err = reg_read(REG_CTRL1_XL, &ctrl1);

	if (!err) {
		err = read_axes(REG_OUTX_L_XL, axes);
	}
	if (err) {
		return err;
	}

	scale = accel_mg_per_lsb[(ctrl1 & CTRL_FS_MASK) >> CTRL_FS_SHIFT] / 1000.0 *
		STANDARD_GRAVITY;
	shell_print(sh, "accel x=%.2f y=%.2f z=%.2f m/s^2",
		    axes[0] * scale, axes[1] * scale, axes[2] * scale);

	return 0;
}

static int cmd_accel(const struct shell *sh, size_t argc, char **argv)
{
	return check_ready(sh) ? report(sh, print_accel(sh)) : -ENODEV;
}

SHELL_CMD_REGISTER(accel, NULL, "Read the on-board accelerometer", cmd_accel);

static int cmd_read(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t ctrl2;
	uint8_t raw_temp[2];
	int16_t axes[3];
	double scale;
	int err;

	if (!check_ready(sh)) {
		return -ENODEV;
	}

	err = print_accel(sh);
	if (!err) {
		err = reg_read(REG_CTRL2_G, &ctrl2);
	}
	if (!err) {
		err = read_axes(REG_OUTX_L_G, axes);
	}
	if (!err) {
		err = i2c_burst_read_dt(&bus, REG_OUT_TEMP_L, raw_temp, sizeof(raw_temp));
	}
	if (err) {
		return report(sh, err);
	}

	scale = gyro_sensitivity(ctrl2) / 1000.0;
	shell_print(sh, "gyro  x=%.2f y=%.2f z=%.2f dps",
		    axes[0] * scale, axes[1] * scale, axes[2] * scale);
	shell_print(sh, "temp  %.1f C", 25.0 + (int16_t)sys_get_le16(raw_temp) / 256.0);

	return 0;
}

static const char *odr_name(uint8_t ctrl)
{
	uint8_t code = ctrl >> CTRL_ODR_SHIFT;

	return code < ARRAY_SIZE(odr_names) ? odr_names[code] : "?";
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t ctrl1;
	uint8_t ctrl2;
	uint8_t ctrl6;
	uint8_t ctrl7;
	int err;

	if (!check_ready(sh)) {
		return -ENODEV;
	}

	err = reg_read(REG_CTRL1_XL, &ctrl1);
	if (!err) {
		err = reg_read(REG_CTRL2_G, &ctrl2);
	}
	if (!err) {
		err = reg_read(REG_CTRL6_C, &ctrl6);
	}
	if (!err) {
		err = reg_read(REG_CTRL7_G, &ctrl7);
	}
	if (err) {
		return report(sh, err);
	}

	shell_print(sh, "accel  odr %s Hz, range +-%u g, %s mode", odr_name(ctrl1),
		    accel_range_g[(ctrl1 & CTRL_FS_MASK) >> CTRL_FS_SHIFT],
		    (ctrl6 & CTRL6_XL_LOW_POWER) ? "low-power" : "high-performance");
	shell_print(sh, "gyro   odr %s Hz, range +-%u dps, %s mode", odr_name(ctrl2),
		    (ctrl2 & CTRL2_FS_125) ? 125
			: gyro_range_dps[(ctrl2 & CTRL_FS_MASK) >> CTRL_FS_SHIFT],
		    (ctrl7 & CTRL7_G_LOW_POWER) ? "low-power" : "high-performance");
	shell_print(sh, "saved  %s", have_saved_config ? "yes" : "no (defaults at boot)");

	return 0;
}

static int set_odr(const struct shell *sh, enum sensor sensor, const char *arg)
{
	if (!check_ready(sh)) {
		return -ENODEV;
	}

	ARRAY_FOR_EACH(odr_names, code) {
		if (strcmp(arg, odr_names[code]) == 0) {
			return report(sh, reg_update(sensor == ACCEL ? REG_CTRL1_XL : REG_CTRL2_G,
						     0xf0, code << CTRL_ODR_SHIFT));
		}
	}

	shell_error(sh, "Rate must be one of: 0 (off) 12.5 26 52 104 208 416 833 1660 3330 6660");
	return -EINVAL;
}

static int set_range(const struct shell *sh, enum sensor sensor, const char *arg)
{
	int err = 0;
	unsigned long range = shell_strtoul(arg, 10, &err);

	if (!check_ready(sh)) {
		return -ENODEV;
	}

	if (!err && sensor == ACCEL) {
		ARRAY_FOR_EACH(accel_range_g, fs) {
			if (range == accel_range_g[fs]) {
				return report(sh, reg_update(REG_CTRL1_XL, CTRL_FS_MASK,
							     fs << CTRL_FS_SHIFT));
			}
		}
	} else if (!err && range == 125) {
		return report(sh, reg_update(REG_CTRL2_G, CTRL_FS_MASK | CTRL2_FS_125,
					     CTRL2_FS_125));
	} else if (!err) {
		ARRAY_FOR_EACH(gyro_range_dps, fs) {
			if (range == gyro_range_dps[fs]) {
				return report(sh, reg_update(REG_CTRL2_G,
							     CTRL_FS_MASK | CTRL2_FS_125,
							     fs << CTRL_FS_SHIFT));
			}
		}
	}

	shell_error(sh, sensor == ACCEL ? "Range must be 2, 4, 8 or 16 (g)"
					: "Range must be 125, 250, 500, 1000 or 2000 (dps)");
	return -EINVAL;
}

static int set_mode(const struct shell *sh, enum sensor sensor, const char *arg)
{
	bool low;

	if (!check_ready(sh)) {
		return -ENODEV;
	}

	if (strcmp(arg, "low") == 0) {
		low = true;
	} else if (strcmp(arg, "high") == 0) {
		low = false;
	} else {
		shell_error(sh, "Mode must be high (high-performance) or low (low-power)");
		return -EINVAL;
	}

	if (sensor == ACCEL) {
		return report(sh, reg_update(REG_CTRL6_C, CTRL6_XL_LOW_POWER,
					     low ? CTRL6_XL_LOW_POWER : 0));
	}
	return report(sh, reg_update(REG_CTRL7_G, CTRL7_G_LOW_POWER,
				     low ? CTRL7_G_LOW_POWER : 0));
}

static int cmd_accel_odr(const struct shell *sh, size_t argc, char **argv)
{
	return set_odr(sh, ACCEL, argv[1]);
}

static int cmd_accel_range(const struct shell *sh, size_t argc, char **argv)
{
	return set_range(sh, ACCEL, argv[1]);
}

static int cmd_accel_mode(const struct shell *sh, size_t argc, char **argv)
{
	return set_mode(sh, ACCEL, argv[1]);
}

static int cmd_gyro_odr(const struct shell *sh, size_t argc, char **argv)
{
	return set_odr(sh, GYRO, argv[1]);
}

static int cmd_gyro_range(const struct shell *sh, size_t argc, char **argv)
{
	return set_range(sh, GYRO, argv[1]);
}

static int cmd_gyro_mode(const struct shell *sh, size_t argc, char **argv)
{
	return set_mode(sh, GYRO, argv[1]);
}

static int parse_reg(const struct shell *sh, const char *arg, unsigned long max,
		     const char *what, unsigned long *value)
{
	int err = 0;

	*value = shell_strtoul(arg, 0, &err);
	if (err || *value > max) {
		shell_error(sh, "%s must be 0 to 0x%lx", what, max);
		return -EINVAL;
	}
	return 0;
}

static int cmd_reg_read(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long reg;
	unsigned long count = 1;
	uint8_t value;
	int err;

	if (!check_ready(sh)) {
		return -ENODEV;
	}
	if (parse_reg(sh, argv[1], REG_LAST, "Register", &reg) ||
	    (argc > 2 && parse_reg(sh, argv[2], REG_LAST + 1, "Count", &count))) {
		return -EINVAL;
	}

	for (; count > 0 && reg <= REG_LAST; count--, reg++) {
		err = reg_read(reg, &value);
		if (err) {
			return report(sh, err);
		}
		shell_print(sh, "0x%02lx = 0x%02x", reg, value);
	}

	return 0;
}

static int cmd_reg_write(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long reg;
	unsigned long value;

	if (!check_ready(sh)) {
		return -ENODEV;
	}
	if (parse_reg(sh, argv[1], REG_LAST, "Register", &reg) ||
	    parse_reg(sh, argv[2], 0xff, "Value", &value)) {
		return -EINVAL;
	}

	return report(sh, reg_write(reg, value));
}

static int cmd_save(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t config[SAVED_SIZE];
	uint8_t *p = config;
	int err;

	if (!check_ready(sh)) {
		return -ENODEV;
	}

	ARRAY_FOR_EACH(saved_ranges, i) {
		err = i2c_burst_read_dt(&bus, saved_ranges[i].start, p, saved_ranges[i].count);
		if (err) {
			return report(sh, err);
		}
		p += saved_ranges[i].count;
	}

	err = settings_save_one("imu/config", config, sizeof(config));
	if (err) {
		shell_error(sh, "Could not save (err %d)", err);
		return err;
	}

	memcpy(saved_config, config, sizeof(config));
	have_saved_config = true;
	shell_print(sh, "Saved; applied again at every boot");

	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	if (!check_ready(sh)) {
		return -ENODEV;
	}

	err = settings_delete("imu/config");
	if (err) {
		shell_error(sh, "Could not delete the saved configuration (err %d)", err);
		return err;
	}
	have_saved_config = false;

	err = software_reset();
	if (!err) {
		err = apply_default_config();
	}
	return report(sh, err);
}

#define SENSOR_SUBCMDS(name, unit, ranges)                                              \
	SHELL_STATIC_SUBCMD_SET_CREATE(name##_cmds,                                     \
		SHELL_CMD_ARG(odr, NULL,                                                \
			      "<Hz>  0 (off) 12.5 26 52 104 208 416 833 1660 3330 6660", \
			      cmd_##name##_odr, 2, 0),                                  \
		SHELL_CMD_ARG(range, NULL, "<" unit ">  " ranges, cmd_##name##_range, 2, 0), \
		SHELL_CMD_ARG(mode, NULL, "<high|low>  High-performance or low-power",  \
			      cmd_##name##_mode, 2, 0),                                 \
		SHELL_SUBCMD_SET_END                                                    \
	)

SENSOR_SUBCMDS(accel, "g", "2 4 8 16");
SENSOR_SUBCMDS(gyro, "dps", "125 250 500 1000 2000");

SHELL_STATIC_SUBCMD_SET_CREATE(reg_cmds,
	SHELL_CMD_ARG(read, NULL, "<register> [count]", cmd_reg_read, 2, 1),
	SHELL_CMD_ARG(write, NULL, "<register> <value>", cmd_reg_write, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(imu_cmds,
	SHELL_CMD(read, NULL, "Read acceleration, rotation rate and temperature", cmd_read),
	SHELL_CMD(accel, &accel_cmds, "Accelerometer rate, range and power mode", NULL),
	SHELL_CMD(gyro, &gyro_cmds, "Gyroscope rate, range and power mode", NULL),
	SHELL_CMD(reg, &reg_cmds, "Read or write any register of the chip", NULL),
	SHELL_CMD(save, NULL, "Keep the current configuration across resets", cmd_save),
	SHELL_CMD(reset, NULL, "Return to the default configuration and forget the saved one",
		  cmd_reset),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(imu, &imu_cmds, "Show or change the IMU configuration", cmd_status);
