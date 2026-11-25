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
 *
 * A bond is known by its address, which says little, so each can be given a
 * name of up to BOND_NAME_MAX characters ("bonds name"), kept with it. A bond
 * without one takes the name the device itself gives: phones and computers
 * have a Device Name characteristic, which is read when they connect.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include "app.h"

LOG_MODULE_REGISTER(bonds, LOG_LEVEL_INF);

#define BOND_NAME_MAX 14

/* A name given to a bonded device. Stored as it is, under "app/bond/<slot>". */
struct bond_name {
	bt_addr_le_t addr;
	char name[BOND_NAME_MAX + 1]; /* empty: the slot is free */
} __packed;

static struct bond_name bond_names[CONFIG_BT_MAX_PAIRED];

static bool pairing_enabled = true;

static struct bond_name *bond_name_find(const bt_addr_le_t *addr)
{
	ARRAY_FOR_EACH_PTR(bond_names, entry) {
		if (entry->name[0] != '\0' && bt_addr_le_eq(&entry->addr, addr)) {
			return entry;
		}
	}
	return NULL;
}

static int bond_name_store(const struct bond_name *entry)
{
	char key[sizeof("app/bond/") + 3];

	snprintf(key, sizeof(key), "app/bond/%u", (unsigned int)(entry - bond_names) & 0xff);

	return entry->name[0] != '\0' ? settings_save_one(key, entry, sizeof(*entry))
				      : settings_delete(key);
}

/* Sets the name of a bonded device; an empty name takes it away. */
static int bond_name_set(const bt_addr_le_t *addr, const char *name)
{
	struct bond_name *entry = bond_name_find(addr);

	if (entry == NULL) {
		if (name[0] == '\0') {
			return 0;
		}
		ARRAY_FOR_EACH_PTR(bond_names, candidate) {
			if (candidate->name[0] == '\0') {
				entry = candidate;
				break;
			}
		}
		if (entry == NULL) {
			/* One slot per bond, and names go when bonds do. */
			return -ENOMEM;
		}
		bt_addr_le_copy(&entry->addr, addr);
	}

	strncpy(entry->name, name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';

	return bond_name_store(entry);
}

/* A removed bond takes its name with it. */
static void bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
	(void)bond_name_set(peer, "");
}

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

/* -------------------------------------------- the device's own name --- */

static void find_bond(const struct bt_bond_info *info, void *user_data)
{
	const bt_addr_le_t **wanted = user_data;

	if (*wanted != NULL && bt_addr_le_eq(&info->addr, *wanted)) {
		*wanted = NULL;
	}
}

static bool is_bonded(const bt_addr_le_t *addr)
{
	const bt_addr_le_t *wanted = addr;

	bt_foreach_bond(BT_ID_DEFAULT, find_bond, &wanted);
	return wanted == NULL;
}

/* One read at a time for each connection. */
static struct bt_gatt_read_params name_reads[CONFIG_BT_MAX_CONN];
static bool name_read_busy[CONFIG_BT_MAX_CONN];

static uint8_t peer_name_read(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_read_params *params, const void *data,
			      uint16_t length)
{
	const bt_addr_le_t *addr = bt_conn_get_dst(conn);
	const uint8_t *bytes = data;
	char name[BOND_NAME_MAX + 1];
	size_t len = 0;

	name_read_busy[bt_conn_index(conn)] = false;

	if (err || data == NULL) {
		/* No such characteristic, or the device keeps it to itself. */
		return BT_GATT_ITER_STOP;
	}

	/*
	 * As much as fits, and only what "bonds" can show: plain characters.
	 * Names are UTF-8, so anything else (a curly apostrophe, say) is left out.
	 */
	for (uint16_t i = 0; i < length && len < BOND_NAME_MAX; i++) {
		if (bytes[i] >= ' ' && bytes[i] < 0x7f && bytes[i] != '"') {
			name[len++] = bytes[i];
		}
	}
	while (len > 0 && name[len - 1] == ' ') {
		len--;
	}
	name[len] = '\0';

	/* Only a default: a name already given stays. */
	if (len > 0 && is_bonded(addr) && bond_name_find(addr) == NULL) {
		if (bond_name_set(addr, name) == 0) {
			LOG_INF("Paired device named \"%s\", as it calls itself", name);
		}
	}

	return BT_GATT_ITER_STOP;
}

/* Asks a bonded device that has no name yet what it calls itself. */
static void peer_name_request(struct bt_conn *conn)
{
	uint8_t index = bt_conn_index(conn);
	struct bt_gatt_read_params *params = &name_reads[index];
	const bt_addr_le_t *addr = bt_conn_get_dst(conn);

	if (name_read_busy[index] || !is_bonded(addr) || bond_name_find(addr) != NULL) {
		return;
	}

	*params = (struct bt_gatt_read_params){
		.func = peer_name_read,
		.handle_count = 0,
		.by_uuid = {
			.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE,
			.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE,
			.uuid = BT_UUID_GAP_DEVICE_NAME,
		},
	};

	name_read_busy[index] = true;
	if (bt_gatt_read(conn, params) != 0) {
		name_read_busy[index] = false;
	}
}

/* A bonded device reconnecting: the link is encrypted with its bond's key. */
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (!err && level >= BT_SECURITY_L2) {
		peer_name_request(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	name_read_busy[bt_conn_index(conn)] = false;
}

BT_CONN_CB_DEFINE(bond_conn_callbacks) = {
	.disconnected = disconnected,
	.security_changed = security_changed,
};

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

	/* The bond exists only now, after the link was encrypted. */
	if (bonded) {
		peer_name_request(conn);
	}
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
	.bond_deleted = bond_deleted,
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
	const char *slot;
	uint8_t value;
	ssize_t rc;

	if (settings_name_steq(name, "bond", &slot) && slot != NULL) {
		struct bond_name entry;
		int index = atoi(slot);

		if (index < 0 || index >= ARRAY_SIZE(bond_names) || len != sizeof(entry)) {
			return 0;
		}
		rc = read_cb(cb_arg, &entry, sizeof(entry));
		if (rc < 0) {
			return rc;
		}
		entry.name[sizeof(entry.name) - 1] = '\0';
		bond_names[index] = entry;
		return 0;
	}

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

	const struct bond_name *named = bond_name_find(&info->addr);

	bt_addr_le_to_str(&info->addr, addr, sizeof(addr));
	shell_print(walk->sh, "%d  %s%s%s%s%s", walk->index, addr,
		    is_connected(&info->addr) ? "  (connected)" : "",
		    named ? "  \"" : "", named ? named->name : "", named ? "\"" : "");
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

static int cmd_bonds_name(const struct shell *sh, size_t argc, char **argv)
{
	struct bond_walk walk = { .sh = sh };
	char name[BOND_NAME_MAX + 1] = "";
	size_t len = 0;
	int err = 0;

	walk.wanted = shell_strtol(argv[1], 10, &err);
	if (err || walk.wanted < 1) {
		shell_error(sh, "Give a bond number from the \"bonds\" list");
		return -EINVAL;
	}

	/* The name may contain spaces: join the remaining arguments. */
	for (size_t i = 2; i < argc; i++) {
		size_t word = strlen(argv[i]);

		if (len + (i > 2 ? 1 : 0) + word > BOND_NAME_MAX) {
			shell_error(sh, "The name can have up to %d characters", BOND_NAME_MAX);
			return -EINVAL;
		}
		if (i > 2) {
			name[len++] = ' ';
		}
		memcpy(&name[len], argv[i], word);
		len += word;
	}
	for (size_t i = 0; i < len; i++) {
		/* The list shows the name in quotes. */
		if (!isprint((unsigned char)name[i]) || name[i] == '"') {
			shell_error(sh, "The name can only have plain printable characters, "
				    "and no quotes");
			return -EINVAL;
		}
	}

	bt_foreach_bond(BT_ID_DEFAULT, walk_bond, &walk);
	if (!walk.have_found) {
		shell_error(sh, "No bond %d", walk.wanted);
		return -ENOENT;
	}

	err = bond_name_set(&walk.found, name);
	if (err) {
		shell_error(sh, "Could not save the name (err %d)", err);
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
	SHELL_CMD_ARG(name, NULL, "<number> [name]  Name a bonded device, or take its name away",
		      cmd_bonds_name, 2, SHELL_OPT_ARG_CHECK_SKIP),
	SHELL_CMD_ARG(remove, NULL, "<number|all>  Forget a bonded device, or all of them",
		      cmd_bonds_remove, 2, 0),
	SHELL_CMD_ARG(pairing, NULL, "<on|off>  Allow or refuse new devices to pair",
		      cmd_bonds_pairing, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bonds, &bonds_cmds, "List bonded Bluetooth devices and control pairing",
		   cmd_bonds);
