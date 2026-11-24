/*
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <bluetooth/services/nus.h>
#include <shell/shell_bt_nus.h>
#include <app_version.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(lsm6ds3tr_c));

static struct k_work adv_work;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}

	LOG_INF("Connected");
	shell_bt_nus_enable(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s", reason, bt_hci_err_to_str(reason));
	shell_bt_nus_disable();
}

static void recycled(void)
{
	/* The connection object is free again: resume advertising. */
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

static int cmd_accel(const struct shell *sh, size_t argc, char **argv)
{
	struct sensor_value accel[3];
	int err;

	if (!device_is_ready(imu)) {
		shell_error(sh, "IMU not ready");
		return -ENODEV;
	}

	err = sensor_sample_fetch(imu);
	if (!err) {
		err = sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, accel);
	}
	if (err) {
		shell_error(sh, "IMU read failed (err %d)", err);
		return err;
	}

	shell_print(sh, "accel x=%.2f y=%.2f z=%.2f m/s^2",
		    sensor_value_to_double(&accel[0]),
		    sensor_value_to_double(&accel[1]),
		    sensor_value_to_double(&accel[2]));

	return 0;
}

SHELL_CMD_REGISTER(accel, NULL, "Read the on-board accelerometer", cmd_accel);

int main(void)
{
	int err;

	LOG_INF("XIAO nRF52840 Sense Plus, firmware %s", APP_VERSION_STRING);

	if (device_is_ready(imu)) {
		struct sensor_value odr = { .val1 = 104 };

		sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
				SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	} else {
		LOG_WRN("IMU not ready");
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth enable failed (err %d)", err);
		return 0;
	}

	err = shell_bt_nus_init();
	if (err) {
		LOG_ERR("Failed to initialize BT NUS shell (err %d)", err);
		return 0;
	}

	k_work_init(&adv_work, adv_work_handler);
	k_work_submit(&adv_work);

	/*
	 * Bluetooth is up, so this image can receive another OTA update: mark
	 * it as good. An image that never gets here is reverted by MCUboot on
	 * the next reset.
	 */
	if (!boot_is_img_confirmed()) {
		err = boot_write_img_confirmed();
		if (err) {
			LOG_ERR("Failed to confirm image (err %d)", err);
		} else {
			LOG_INF("New image confirmed");
		}
	}

	/* The LED is driven from its own thread, see blink.c. */
	return 0;
}
