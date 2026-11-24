/*
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <app_version.h>

#include "app.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* Value the Adafruit bootloader looks for in GPREGRET to stay in UF2 mode. */
#define UF2_BOOTLOADER_MAGIC 0x57

/* How long a new connection gets to encrypt the link before it is dropped. */
#define SECURITY_TIMEOUT_MS 30000

static struct k_work adv_work;
static struct k_work_delayable security_timeout_work;

/* Connections that have not encrypted the link yet, and when they run out of time. */
static struct {
	struct bt_conn *conn;
	int64_t deadline;
} unsecured[CONFIG_BT_MAX_CONN];
static K_MUTEX_DEFINE(unsecured_lock);

static const uint8_t ad_flags[] = { BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };

/* Scan response: the services hosts look for, and what kind of device this is. */
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, APP_UUID_NUS_SERVICE_VAL),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      BT_BYTES_LIST_LE16(CONFIG_BT_DEVICE_APPEARANCE)),
};

/* The advertised name can change at run time, so the data is built on demand. */
static void build_ad(struct bt_data ad[2])
{
	const char *name = bt_get_name();

	ad[0] = (struct bt_data)BT_DATA(BT_DATA_FLAGS, ad_flags, sizeof(ad_flags));
	ad[1] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
}

/* Advertises whenever there is room for another connection. */
static void adv_work_handler(struct k_work *work)
{
	struct bt_data ad[2];
	int err;

	build_ad(ad);
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err && err != -EALREADY && err != -ENOMEM && err != -ECONNREFUSED) {
		LOG_ERR("Advertising failed to start (err %d)", err);
	}
}

static void unsecured_remove(struct bt_conn *conn)
{
	k_mutex_lock(&unsecured_lock, K_FOREVER);
	ARRAY_FOR_EACH(unsecured, i) {
		if (unsecured[i].conn == conn) {
			bt_conn_unref(unsecured[i].conn);
			unsecured[i].conn = NULL;
		}
	}
	k_mutex_unlock(&unsecured_lock);
}

/* Drops connections that have not encrypted in time, so they cannot sit on a slot. */
static void security_timeout_handler(struct k_work *work)
{
	int64_t now = k_uptime_get();
	int64_t next = INT64_MAX;

	k_mutex_lock(&unsecured_lock, K_FOREVER);
	ARRAY_FOR_EACH(unsecured, i) {
		if (unsecured[i].conn == NULL) {
			continue;
		}
		if (unsecured[i].deadline <= now) {
			LOG_WRN("Link not encrypted in time, disconnecting");
			bt_conn_disconnect(unsecured[i].conn, BT_HCI_ERR_AUTH_FAIL);
			/* Removed from the list when the disconnection completes. */
			unsecured[i].deadline = INT64_MAX;
		} else {
			next = MIN(next, unsecured[i].deadline);
		}
	}
	k_mutex_unlock(&unsecured_lock);

	if (next != INT64_MAX) {
		k_work_reschedule(&security_timeout_work, K_MSEC(next - now));
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}

	LOG_INF("Connected");

	err = hid_connected(conn);
	if (err) {
		LOG_WRN("HID service could not take the connection (err %d)", err);
	}

	/*
	 * Ask the peer to encrypt the link: a bonded peer reuses its key, a new
	 * one pairs. The shell, OTA and HID services refuse access until then.
	 */
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Could not request encryption (err %d)", err);
	}

	k_mutex_lock(&unsecured_lock, K_FOREVER);
	ARRAY_FOR_EACH(unsecured, i) {
		if (unsecured[i].conn == NULL) {
			unsecured[i].conn = bt_conn_ref(conn);
			unsecured[i].deadline = k_uptime_get() + SECURITY_TIMEOUT_MS;
			break;
		}
	}
	k_mutex_unlock(&unsecured_lock);
	k_work_schedule(&security_timeout_work, K_MSEC(SECURITY_TIMEOUT_MS));

	/* Twice the data rate where the host supports it; harmless where not. */
	(void)bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);

	/* Keep advertising for further devices. */
	k_work_submit(&adv_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (err || level < BT_SECURITY_L2) {
		LOG_WRN("Link not secured (%s), disconnecting", bt_security_err_to_str(err));
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}

	LOG_INF("Link encrypted");
	unsecured_remove(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s", reason, bt_hci_err_to_str(reason));

	unsecured_remove(conn);
	hid_disconnected(conn);
}

/* Device Information Service serial number: the chip's unique ID, in hex. */
static void set_serial_number(void)
{
	uint8_t id[8];
	char serial[2 * sizeof(id) + 1];
	ssize_t len = hwinfo_get_device_id(id, sizeof(id));
	int err;

	if (len <= 0) {
		LOG_WRN("No device ID for the serial number (err %d)", (int)len);
		return;
	}

	bin2hex(id, len, serial, sizeof(serial));
	err = settings_runtime_set("bt/dis/serial", serial, strlen(serial));
	if (err) {
		LOG_WRN("Could not set the serial number (err %d)", err);
	} else {
		LOG_INF("Serial number %s", serial);
	}
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	LOG_INF("Connection interval %u.%02u ms", interval * 5 / 4, (interval * 125) % 100);
}

static void recycled(void)
{
	/* The connection object is free again: resume advertising. */
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.le_param_updated = le_param_updated,
	.recycled = recycled,
};

static int cmd_name(const struct shell *sh, size_t argc, char **argv)
{
	char name[CONFIG_BT_DEVICE_NAME_MAX + 1] = "";
	struct bt_data ad[2];
	size_t len = 0;
	int err;

	if (argc == 1) {
		shell_print(sh, "%s", bt_get_name());
		return 0;
	}

	/* Unquoted words are joined with single spaces. */
	for (size_t i = 1; i < argc; i++) {
		size_t word = strlen(argv[i]);

		if (len + (i > 1) + word > CONFIG_BT_DEVICE_NAME_MAX) {
			shell_error(sh, "Name must be at most %d characters",
				    CONFIG_BT_DEVICE_NAME_MAX);
			return -EINVAL;
		}
		if (i > 1) {
			name[len++] = ' ';
		}
		memcpy(&name[len], argv[i], word);
		len += word;
	}

	for (size_t i = 0; i < len; i++) {
		if (!isprint((unsigned char)name[i])) {
			shell_error(sh, "Name must be plain printable characters");
			return -EINVAL;
		}
	}
	if (len == 0) {
		shell_error(sh, "Name must not be empty");
		return -EINVAL;
	}

	err = bt_set_name(name);
	if (err) {
		shell_error(sh, "Could not set the name (err %d)", err);
		return err;
	}

	/* Takes effect now if advertising; otherwise when advertising next starts. */
	build_ad(ad);
	(void)bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	/* Bluetooth stores the name in settings itself. */
	return 0;
}

SHELL_CMD_ARG_REGISTER(name, NULL, "[new name]  Show or change the board's Bluetooth name",
		       cmd_name, 1, SHELL_OPT_ARG_CHECK_SKIP);

static int cmd_bootloader(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Resetting into the UF2 bootloader...");
	/* Let the message reach the terminal before the link drops. */
	k_msleep(500);

	nrf_power_gpregret_set(NRF_POWER, 0, UF2_BOOTLOADER_MAGIC);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_CMD_REGISTER(bootloader, NULL,
		   "Reset into the UF2 bootloader (USB drive) for flashing over USB",
		   cmd_bootloader);

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Restarting...");
	/* Let the message reach the terminal before the link drops. */
	k_msleep(500);

	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_CMD_REGISTER(reboot, NULL, "Restart the board", cmd_reboot);

/*
 * Everything the board remembers (name, blink, IMU and HID settings, pairing
 * switch and the bonds themselves) lives in the storage partition, so erasing
 * it and restarting brings back every default at once.
 */
static int cmd_factory_reset(const struct shell *sh, size_t argc, char **argv)
{
	const struct flash_area *storage;
	int err;

	if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
		shell_error(sh, "This erases all settings and paired devices. "
				"To go ahead: factory_reset confirm");
		return -EINVAL;
	}

	shell_print(sh, "Erasing all settings and paired devices, then restarting...");
	k_msleep(500);

	err = flash_area_open(PARTITION_ID(storage_partition), &storage);
	if (!err) {
		err = flash_area_flatten(storage, 0, storage->fa_size);
		flash_area_close(storage);
	}
	if (err) {
		shell_error(sh, "Could not erase the settings (err %d)", err);
		return err;
	}

	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_CMD_ARG_REGISTER(factory_reset, NULL,
		       "confirm  Erase all settings and paired devices, then restart",
		       cmd_factory_reset, 1, 1);

int main(void)
{
	uint32_t reset_cause;
	int err;

	LOG_INF("XIAO nRF52840 Sense Plus, firmware %s", APP_VERSION_STRING);
	crash_report();

	/*
	 * Application settings first; the Bluetooth ones (bonds, name) can only
	 * be loaded once Bluetooth is enabled.
	 */
	err = settings_subsys_init();
	if (!err) {
		err = settings_load_subtree("app");
	}
	if (!err) {
		err = settings_load_subtree("blink");
	}
	if (!err) {
		err = settings_load_subtree("imu");
	}
	if (!err) {
		err = settings_load_subtree("hid");
	}
	if (err) {
		LOG_ERR("Could not load settings, using defaults (err %d)", err);
	}

	/* The only pin armed to wake the SoC from sleep is the IMU's interrupt. */
	if (hwinfo_get_reset_cause(&reset_cause) == 0) {
		if (reset_cause & RESET_LOW_POWER_WAKE) {
			LOG_INF("Woken from sleep by motion");
		}
		hwinfo_clear_reset_cause();
	}

	imu_init();

	k_work_init(&adv_work, adv_work_handler);
	k_work_init_delayable(&security_timeout_work, security_timeout_handler);

	err = bonds_init();
	if (err) {
		LOG_ERR("Could not set up pairing (err %d)", err);
		return 0;
	}

	err = hid_init();
	if (err) {
		LOG_ERR("Could not set up the HID service (err %d)", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth enable failed (err %d)", err);
		return 0;
	}

	err = settings_load_subtree("bt");
	if (err) {
		LOG_ERR("Could not load Bluetooth settings (err %d)", err);
	}
	set_serial_number();

	err = ble_shell_init();
	if (err) {
		LOG_ERR("Failed to start the Bluetooth shell (err %d)", err);
		return 0;
	}

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
