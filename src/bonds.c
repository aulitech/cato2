/*
 * SPDX-License-Identifier: MIT
 *
 * Bluetooth pairing policy and the "bonds" shell command.
 *
 * The console and OTA services need an encrypted link, so a phone or computer
 * has to pair (bond) with the board before it can use them. Up to
 * CONFIG_BT_MAX_PAIRED bonds are kept across resets. New pairings are accepted
 * while there is a free slot and pairing has not been switched off with
 * "bonds pairing off".
 *
 * The board has no display or buttons, so pairing is "Just Works": the link is
 * encrypted, but nothing proves who is pairing. Switch pairing off once your
 * devices are bonded.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include "app.h"

LOG_MODULE_REGISTER(bonds, LOG_LEVEL_INF);

static bool pairing_enabled = true;

static void count_bond(const struct bt_bond_info *info, void *user_data)
{
	(*(int *)user_data)++;
}

static int bond_count(void)
{
	int count = 0;

	bt_foreach_bond(BT_ID_DEFAULT, count_bond, &count);
	return count;
}

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *const feat)
{
	if (!pairing_enabled) {
		LOG_WRN("Pairing refused: switched off");
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}
	if (bond_count() >= CONFIG_BT_MAX_PAIRED) {
		LOG_WRN("Pairing refused: all %d bonds in use", CONFIG_BT_MAX_PAIRED);
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}

	return BT_SECURITY_ERR_SUCCESS;
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Paired with %s (%d of %d bonds)", addr, bond_count(), CONFIG_BT_MAX_PAIRED);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("Pairing failed: %s", bt_security_err_to_str(reason));
}

static struct bt_conn_auth_cb auth_callbacks = {
	.pairing_accept = pairing_accept,
};

static struct bt_conn_auth_info_cb auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

int bonds_init(void)
{
	int err = bt_conn_auth_cb_register(&auth_callbacks);

	if (!err) {
		err = bt_conn_auth_info_cb_register(&auth_info_callbacks);
	}
	return err;
}

/* ------------------------------------------------------------ settings --- */

static int app_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			    void *cb_arg)
{
	uint8_t value;
	ssize_t rc;

	if (!settings_name_steq(name, "pairing", NULL)) {
		/* Other "app/..." keys from older firmware are ignored. */
		return 0;
	}
	if (len != sizeof(value)) {
		return -EINVAL;
	}

	rc = read_cb(cb_arg, &value, sizeof(value));
	if (rc < 0) {
		return rc;
	}
	pairing_enabled = value != 0;

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(app, "app", NULL, app_settings_set, NULL, NULL);

/* --------------------------------------------------------------- shell --- */

struct bond_walk {
	const struct shell *sh;
	int index;        /* Numbering used by "bonds" and "bonds remove". */
	int wanted;       /* Index to look up, or 0 to print them all. */
	bt_addr_le_t found;
	bool have_found;
};

static bool is_connected(const bt_addr_le_t *addr)
{
	struct bt_conn *conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
	struct bt_conn_info info;
	bool connected = false;

	if (conn != NULL) {
		connected = bt_conn_get_info(conn, &info) == 0 &&
			    info.state == BT_CONN_STATE_CONNECTED;
		bt_conn_unref(conn);
	}
	return connected;
}

static void walk_bond(const struct bt_bond_info *info, void *user_data)
{
	struct bond_walk *walk = user_data;
	char addr[BT_ADDR_LE_STR_LEN];

	walk->index++;

	if (walk->wanted != 0) {
		if (walk->index == walk->wanted) {
			bt_addr_le_copy(&walk->found, &info->addr);
			walk->have_found = true;
		}
		return;
	}

	bt_addr_le_to_str(&info->addr, addr, sizeof(addr));
	shell_print(walk->sh, "%d  %s%s", walk->index, addr,
		    is_connected(&info->addr) ? "  (connected)" : "");
}

static int cmd_bonds(const struct shell *sh, size_t argc, char **argv)
{
	struct bond_walk walk = { .sh = sh };

	bt_foreach_bond(BT_ID_DEFAULT, walk_bond, &walk);

	shell_print(sh, "%d of %d bonds used, pairing %s", walk.index, CONFIG_BT_MAX_PAIRED,
		    !pairing_enabled ? "off"
		    : walk.index >= CONFIG_BT_MAX_PAIRED ? "on (but no free slot)" : "on");

	return 0;
}

static int cmd_bonds_remove(const struct shell *sh, size_t argc, char **argv)
{
	struct bond_walk walk = { .sh = sh };
	char addr[BT_ADDR_LE_STR_LEN];
	int err = 0;

	if (strcmp(argv[1], "all") == 0) {
		err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
		if (err) {
			shell_error(sh, "Could not remove the bonds (err %d)", err);
		}
		return err;
	}

	walk.wanted = shell_strtol(argv[1], 10, &err);
	if (err || walk.wanted < 1) {
		shell_error(sh, "Give a bond number from the \"bonds\" list, or \"all\"");
		return -EINVAL;
	}

	bt_foreach_bond(BT_ID_DEFAULT, walk_bond, &walk);
	if (!walk.have_found) {
		shell_error(sh, "No bond %d", walk.wanted);
		return -ENOENT;
	}

	/* Removing the bond of the connected device also disconnects it. */
	bt_addr_le_to_str(&walk.found, addr, sizeof(addr));
	shell_print(sh, "Removing %s", addr);

	err = bt_unpair(BT_ID_DEFAULT, &walk.found);
	if (err) {
		shell_error(sh, "Could not remove the bond (err %d)", err);
	}
	return err;
}

static int cmd_bonds_pairing(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t value;
	int err;

	if (strcmp(argv[1], "on") == 0) {
		value = 1;
	} else if (strcmp(argv[1], "off") == 0) {
		value = 0;
	} else {
		shell_error(sh, "Use on or off");
		return -EINVAL;
	}

	pairing_enabled = value != 0;

	err = settings_save_one("app/pairing", &value, sizeof(value));
	if (err) {
		shell_error(sh, "Applied, but not saved (err %d)", err);
	}
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(bonds_cmds,
	SHELL_CMD_ARG(remove, NULL, "<number|all>  Forget a bonded device, or all of them",
		      cmd_bonds_remove, 2, 0),
	SHELL_CMD_ARG(pairing, NULL, "<on|off>  Allow or refuse new devices to pair",
		      cmd_bonds_pairing, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bonds, &bonds_cmds, "List bonded Bluetooth devices and control pairing",
		   cmd_bonds);
