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
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <app_version.h>

#include "app.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* Value the Adafruit bootloader looks for in GPREGRET to stay in UF2 mode. */
#define UF2_BOOTLOADER_MAGIC 0x57

/* How long a new connection gets to encrypt the link before it is dropped. */
#define SECURITY_TIMEOUT K_SECONDS(30)

static struct k_work adv_work;
static struct k_work_delayable security_timeout_work;
static struct bt_conn *current_conn;
static bool link_encrypted;

static const uint8_t ad_flags[] = { BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, APP_UUID_NUS_SERVICE_VAL),
};

/* The advertised name can change at run time, so the data is built on demand. */
static void build_ad(struct bt_data ad[2])
{
	const char *name = bt_get_name();

	ad[0] = (struct bt_data)BT_DATA(BT_DATA_FLAGS, ad_flags, sizeof(ad_flags));
	ad[1] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
}

static void adv_work_handler(struct k_work *work)
{
	struct bt_data ad[2];
	int err;

	build_ad(ad);
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
	}
}

static void security_timeout_handler(struct k_work *work)
{
	if (current_conn != NULL && !link_encrypted) {
		LOG_WRN("Link not encrypted in time, disconnecting");
		bt_conn_disconnect(current_conn, BT_HCI_ERR_AUTH_FAIL);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}

	LOG_INF("Connected");
	current_conn = bt_conn_ref(conn);
	link_encrypted = false;

	ble_shell_connected(conn);

	/*
	 * Ask the peer to encrypt the link: a bonded peer reuses its key, a new
	 * one pairs. The shell and OTA services refuse access until then.
	 */
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Could not request encryption (err %d)", err);
	}
	k_work_schedule(&security_timeout_work, SECURITY_TIMEOUT);
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
	link_encrypted = true;
	k_work_cancel_delayable(&security_timeout_work);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s", reason, bt_hci_err_to_str(reason));

	k_work_cancel_delayable(&security_timeout_work);
	if (current_conn != NULL) {
		ble_shell_disconnected();
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
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

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth enable failed (err %d)", err);
		return 0;
	}

	err = settings_load_subtree("bt");
	if (err) {
		LOG_ERR("Could not load Bluetooth settings (err %d)", err);
	}

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
