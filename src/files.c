/*
 * SPDX-License-Identifier: MIT
 *
 * Files: a LittleFS file system on the board's 2 MB QSPI flash, mounted at
 * /lfs. The firmware reads and writes it with files_read() and files_write(),
 * a shell session with the "files" command, and MyCato over Bluetooth with
 * MCUmgr's file transfer, which sends and fetches a file in pieces.
 *
 * Several of those can be at it at once: two shell sessions (USB and
 * Bluetooth), up to three Bluetooth connections, and the firmware itself.
 * What keeps them from tripping over each other:
 *
 *  - One lock. Everything here that takes more than a single file system call
 *    holds it from start to finish: listing, checksumming, storing, removing,
 *    and the firmware's own reads and writes. A transfer over Bluetooth has to
 *    get it to begin.
 *
 *  - One transfer at a time. MCUmgr keeps a single file open; left alone, a
 *    second transfer would quietly take over from the first. Here a transfer
 *    that would get in the way of one under way is refused as busy instead. A
 *    transfer that has gone quiet (its link dropped) stops counting after
 *    TRANSFER_STALE_MS, by when MCUmgr has closed its file.
 *
 *  - Nothing is written in place. An upload goes to "<name>.part", never to
 *    the file itself, and becomes the file only by "files commit", which
 *    checks its size and CRC-32 against what the sender says it sent and then
 *    renames it over the old one. LittleFS renames atomically: at every
 *    moment, and after a power cut at any moment, the name holds either the
 *    whole old file or the whole new one. The firmware's own writes go the
 *    same way, through "<name>.tmp".
 *
 *  - A file in use stays put. It cannot be replaced or removed while it is
 *    being downloaded, nor an upload stored or discarded while it is still
 *    arriving. A reader can check all the same: "files crc" before a download
 *    gives the checksum that what arrives must have.
 *
 *  - Only plain names. Bluetooth and the shell reach the files directly in
 *    /lfs with names of NAME_CHARS: no directories, no way out of /lfs, and
 *    no reaching the ".part" and ".tmp" files except as described.
 *
 *  - Leftovers do not last. What an interrupted upload or write left behind is
 *    removed when the board starts.
 *
 * LittleFS itself survives power loss and spreads wear. The flash chip sleeps
 * (deep power-down) whenever it has been idle for a moment.
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include "app.h"

LOG_MODULE_REGISTER(files, LOG_LEVEL_INF);

#define MOUNT_POINT "/lfs"
#define MOUNT_PREFIX MOUNT_POINT "/"

#define NAME_MAX_LEN 40 /* of a file's name, as the user sees it */
#define NAME_CHARS "letters, digits, '.', '_' and '-'"
#define PART_SUFFIX ".part" /* an upload that is still arriving, or not stored yet */
#define TMP_SUFFIX ".tmp"   /* a write by the firmware that is not finished */
#define SUFFIX_MAX_LEN 5
#define PATH_MAX_LEN (sizeof(MOUNT_PREFIX) - 1 + NAME_MAX_LEN + SUFFIX_MAX_LEN)

/* MCUmgr closes a transfer's file after 4 s without a request. */
BUILD_ASSERT(CONFIG_MCUMGR_GRP_FS_FILE_AUTOMATIC_IDLE_CLOSE_TIME <= 4000);
#define TRANSFER_STALE_MS 6000

/* How long a transfer over Bluetooth waits for the lock before it is told "busy". */
#define TRANSFER_LOCK_WAIT K_MSEC(1500)

BUILD_ASSERT(PATH_MAX_LEN <= CONFIG_MCUMGR_GRP_FS_PATH_LEN);

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_data);

static struct fs_mount_t mount = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_data,
	.storage_dev = (void *)PARTITION_ID(files_partition),
	.mnt_point = MOUNT_POINT,
};

/* Guards everything below, and every sequence of file system calls in this file. */
static K_MUTEX_DEFINE(lock);

static bool mounted;

/* The transfer MCUmgr has under way, if any. */
static struct {
	bool active;
	bool upload;
	char file[NAME_MAX_LEN + SUFFIX_MAX_LEN + 1]; /* as in /lfs, suffix and all */
	int64_t last_ms; /* when it last made progress */
} transfer;

/* Shared by whoever holds the lock: too big for the stacks it is used from. */
static uint8_t io_buf[256];

static bool has_suffix(const char *name, const char *suffix)
{
	size_t len = strlen(name);
	size_t suffix_len = strlen(suffix);

	return len > suffix_len && strcmp(name + len - suffix_len, suffix) == 0;
}

/* A name a user may give a file: plain, and not one of the reserved endings. */
static bool name_valid(const char *name)
{
	size_t len = strlen(name);

	if (len == 0 || len > NAME_MAX_LEN || !isalnum((unsigned char)name[0])) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = name[i];

		if (!isalnum(c) && c != '.' && c != '_' && c != '-') {
			return false;
		}
	}
	return !has_suffix(name, PART_SUFFIX) && !has_suffix(name, TMP_SUFFIX);
}

static void make_path(char *path, size_t size, const char *name, const char *suffix)
{
	snprintf(path, size, MOUNT_PREFIX "%s%s", name, suffix);
}

/* With the lock held: is a transfer of this file (as named in /lfs) going on? */
static bool transfer_uses(const char *file)
{
	if (!transfer.active) {
		return false;
	}
	if (k_uptime_get() - transfer.last_ms > TRANSFER_STALE_MS) {
		/* Abandoned: MCUmgr has closed its file by now. */
		transfer.active = false;
		return false;
	}
	return strcmp(transfer.file, file) == 0;
}

static bool part_in_use(const char *name)
{
	char file[NAME_MAX_LEN + SUFFIX_MAX_LEN + 1];

	snprintf(file, sizeof(file), "%s" PART_SUFFIX, name);
	return transfer_uses(file);
}

/*
 * With the lock held: is there such a file (as named in /lfs)? Asked of the
 * directory, because opening or removing a file that is not there is logged
 * as an error by the file system, and here it is an ordinary answer.
 */
static bool exists(const char *file)
{
	struct fs_dirent entry;
	struct fs_dir_t dir;
	bool found = false;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, MOUNT_POINT) != 0) {
		return false;
	}
	while (!found && fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		found = entry.type == FS_DIR_ENTRY_FILE && strcmp(entry.name, file) == 0;
	}
	(void)fs_closedir(&dir);

	return found;
}

/* With the lock held: the size and CRC-32 of a file. */
static int checksum(const char *path, size_t *size, uint32_t *crc)
{
	struct fs_file_t file;
	ssize_t got;
	int err;

	if (!exists(path + sizeof(MOUNT_PREFIX) - 1)) {
		return -ENOENT;
	}
	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_READ);
	if (err) {
		return err;
	}

	*size = 0;
	*crc = 0;
	while ((got = fs_read(&file, io_buf, sizeof(io_buf))) > 0) {
		*crc = crc32_ieee_update(*crc, io_buf, got);
		*size += got;
	}
	err = fs_close(&file);

	return got < 0 ? (int)got : err;
}

/* ------------------------------------------------------- for the firmware --- */

int files_read(const char *name, void *buf, size_t size, size_t *len)
{
	char path[PATH_MAX_LEN + 1];
	struct fs_file_t file;
	ssize_t got;
	int err;

	if (!name_valid(name)) {
		return -EINVAL;
	}
	make_path(path, sizeof(path), name, "");

	k_mutex_lock(&lock, K_FOREVER);
	if (!mounted) {
		err = -ENODEV;
		goto out;
	}
	if (!exists(name)) {
		err = -ENOENT;
		goto out;
	}
	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_READ);
	if (err) {
		goto out;
	}
	got = fs_read(&file, buf, size);
	err = fs_close(&file);
	if (got < 0) {
		err = got;
	} else if (!err) {
		*len = got;
	}
out:
	k_mutex_unlock(&lock);
	return err;
}

int files_write(const char *name, const void *data, size_t len)
{
	char path[PATH_MAX_LEN + 1];
	char tmp[PATH_MAX_LEN + 1];
	struct fs_file_t file;
	ssize_t put;
	int err;

	if (!name_valid(name)) {
		return -EINVAL;
	}
	make_path(path, sizeof(path), name, "");
	make_path(tmp, sizeof(tmp), name, TMP_SUFFIX);

	k_mutex_lock(&lock, K_FOREVER);
	if (!mounted) {
		err = -ENODEV;
		goto out;
	}
	if (transfer_uses(name)) {
		/* Being downloaded: it must not change under the reader. */
		err = -EBUSY;
		goto out;
	}

	/* All of it under another name first, and then in place at a stroke. */
	fs_file_t_init(&file);
	err = fs_open(&file, tmp, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (err) {
		goto out;
	}
	put = fs_write(&file, data, len);
	err = fs_close(&file);
	if (put < 0) {
		err = put;
	} else if (!err && (size_t)put != len) {
		err = -ENOSPC;
	}
	if (!err) {
		err = fs_rename(tmp, path);
	}
	if (err) {
		(void)fs_unlink(tmp);
	}
out:
	k_mutex_unlock(&lock);
	return err;
}

/* ------------------------------------------------- transfers over Bluetooth --- */

/*
 * MCUmgr asks before it opens a file for a transfer: once at the start of
 * each, not for every piece. Refusing here is what keeps a transfer to the
 * rules at the top of this file.
 */
static enum mgmt_cb_return transfer_begin(const struct fs_mgmt_file_access *access, int32_t *rc)
{
	const char *file = access->filename + sizeof(MOUNT_PREFIX) - 1;
	bool upload = access->access == FS_MGMT_FILE_ACCESS_WRITE;
	char name[NAME_MAX_LEN + SUFFIX_MAX_LEN + 1];

	*rc = MGMT_ERR_EACCESSDENIED;
	if (access->access != FS_MGMT_FILE_ACCESS_READ && !upload) {
		/* Nothing but sending and fetching is offered. */
		return MGMT_CB_ERROR_RC;
	}
	if (strncmp(access->filename, MOUNT_PREFIX, sizeof(MOUNT_PREFIX) - 1) != 0 ||
	    strlen(file) >= sizeof(name)) {
		return MGMT_CB_ERROR_RC;
	}

	/* An upload may only go to "<name>.part"; anything else only to a file. */
	strcpy(name, file);
	if (upload) {
		if (!has_suffix(name, PART_SUFFIX)) {
			return MGMT_CB_ERROR_RC;
		}
		name[strlen(name) - (sizeof(PART_SUFFIX) - 1)] = '\0';
	}
	if (!name_valid(name)) {
		return MGMT_CB_ERROR_RC;
	}

	*rc = MGMT_ERR_EBUSY;
	if (k_mutex_lock(&lock, TRANSFER_LOCK_WAIT) != 0) {
		return MGMT_CB_ERROR_RC;
	}
	if (!mounted) {
		*rc = MGMT_ERR_ENOENT;
		k_mutex_unlock(&lock);
		return MGMT_CB_ERROR_RC;
	}
	/*
	 * transfer_uses() also drops a transfer that has gone stale. One still
	 * under way is not pushed aside, unless this is the same one starting
	 * over.
	 */
	if (!transfer_uses(file) && transfer.active) {
		k_mutex_unlock(&lock);
		return MGMT_CB_ERROR_RC;
	}

	transfer.active = true;
	transfer.upload = upload;
	strcpy(transfer.file, file);
	transfer.last_ms = k_uptime_get();
	k_mutex_unlock(&lock);

	return MGMT_CB_OK;
}

static enum mgmt_cb_return on_file_access(uint32_t event, enum mgmt_cb_return prev_status,
					  int32_t *rc, uint16_t *group, bool *abort_more,
					  void *data, size_t data_size)
{
	if (event == MGMT_EVT_OP_FS_MGMT_FILE_ACCESS) {
		return transfer_begin(data, rc);
	}

	/* MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE: all of it sent or received, file closed. */
	k_mutex_lock(&lock, K_FOREVER);
	transfer.active = false;
	k_mutex_unlock(&lock);

	return MGMT_CB_OK;
}

/* Each piece of a transfer is a command: one finishing shows the transfer is alive. */
static enum mgmt_cb_return on_command_done(uint32_t event, enum mgmt_cb_return prev_status,
					   int32_t *rc, uint16_t *group, bool *abort_more,
					   void *data, size_t data_size)
{
	const struct mgmt_evt_op_cmd_arg *command = data;

	if (command->group == MGMT_GROUP_ID_FS) {
		k_mutex_lock(&lock, K_FOREVER);
		if (transfer.active) {
			transfer.last_ms = k_uptime_get();
		}
		k_mutex_unlock(&lock);
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback file_access_callback = {
	.callback = on_file_access,
	.event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS | MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE,
};

static struct mgmt_callback command_done_callback = {
	.callback = on_command_done,
	.event_id = MGMT_EVT_OP_CMD_DONE,
};

/* ------------------------------------------------------------------ shell --- */

/* Takes the lock for a command; false, with the reason shown, if there are no files. */
static bool begin(const struct shell *sh)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (!mounted) {
		k_mutex_unlock(&lock);
		shell_error(sh, "The file system is not available");
		return false;
	}
	return true;
}

static int end(int err)
{
	k_mutex_unlock(&lock);
	return err;
}

static bool check_name(const struct shell *sh, const char *name)
{
	if (name_valid(name)) {
		return true;
	}
	shell_error(sh, "A file name is 1 to %d of " NAME_CHARS ", starting with a letter "
			"or digit, and does not end in " PART_SUFFIX " or " TMP_SUFFIX,
		    NAME_MAX_LEN);
	return false;
}

static int cmd_files(const struct shell *sh, size_t argc, char **argv)
{
	struct fs_statvfs stat;
	struct fs_dirent entry;
	struct fs_dir_t dir;
	int count = 0;
	int err;

	if (!begin(sh)) {
		return -ENODEV;
	}

	fs_dir_t_init(&dir);
	err = fs_opendir(&dir, MOUNT_POINT);
	if (err) {
		shell_error(sh, "Could not read the files (err %d)", err);
		return end(err);
	}
	while ((err = fs_readdir(&dir, &entry)) == 0 && entry.name[0] != '\0') {
		/* Uploads not stored yet and unfinished writes are not files. */
		if (entry.type == FS_DIR_ENTRY_FILE && name_valid(entry.name)) {
			shell_print(sh, "%u  %s", (unsigned int)entry.size, entry.name);
			count++;
		}
	}
	(void)fs_closedir(&dir);
	if (!err) {
		err = fs_statvfs(MOUNT_POINT, &stat);
	}
	if (err) {
		shell_error(sh, "Could not read the files (err %d)", err);
		return end(err);
	}

	shell_print(sh, "%d files, %lu of %lu bytes free", count, stat.f_bfree * stat.f_frsize,
		    stat.f_blocks * stat.f_frsize);
	return end(0);
}

static int cmd_crc(const struct shell *sh, size_t argc, char **argv)
{
	char path[PATH_MAX_LEN + 1];
	uint32_t crc;
	size_t size;
	int err;

	if (!check_name(sh, argv[1])) {
		return -EINVAL;
	}
	make_path(path, sizeof(path), argv[1], "");

	if (!begin(sh)) {
		return -ENODEV;
	}
	err = checksum(path, &size, &crc);
	if (err == -ENOENT) {
		shell_error(sh, "No file %s", argv[1]);
	} else if (err) {
		shell_error(sh, "Could not read %s (err %d)", argv[1], err);
	} else {
		shell_print(sh, "%u bytes, crc32 %08x", (unsigned int)size, crc);
	}
	return end(err);
}

/*
 * Makes an upload the file: "<name>.part" becomes "<name>", if it is what
 * the sender says it sent.
 */
static int cmd_commit(const struct shell *sh, size_t argc, char **argv)
{
	const char *name = argv[1];
	char path[PATH_MAX_LEN + 1];
	char part[PATH_MAX_LEN + 1];
	unsigned long want_size;
	unsigned long want_crc;
	uint32_t crc;
	size_t size;
	int err = 0;

	if (!check_name(sh, name)) {
		return -EINVAL;
	}
	want_size = shell_strtoul(argv[2], 10, &err);
	want_crc = shell_strtoul(argv[3], 16, &err);
	if (err) {
		shell_error(sh, "Give the size in bytes and the CRC-32 in hexadecimal");
		return -EINVAL;
	}
	make_path(path, sizeof(path), name, "");
	make_path(part, sizeof(part), name, PART_SUFFIX);

	if (!begin(sh)) {
		return -ENODEV;
	}
	if (part_in_use(name)) {
		shell_error(sh, "The upload of %s has not finished", name);
		return end(-EBUSY);
	}
	if (transfer_uses(name)) {
		shell_error(sh, "%s is being downloaded: try again when that is done", name);
		return end(-EBUSY);
	}

	err = checksum(part, &size, &crc);
	if (err == -ENOENT) {
		shell_error(sh, "No upload of %s to store", name);
		return end(err);
	}
	if (err) {
		shell_error(sh, "Could not read the upload (err %d)", err);
		return end(err);
	}
	if (size != want_size || crc != want_crc) {
		/* Cut short, or two senders at once: not worth keeping. */
		(void)fs_unlink(part);
		shell_error(sh, "The upload is not what was sent (%u bytes, crc32 %08x): "
				"discarded, %s is unchanged", (unsigned int)size, crc, name);
		return end(-EIO);
	}

	err = fs_rename(part, path);
	if (err) {
		shell_error(sh, "Could not store %s (err %d)", name, err);
		return end(err);
	}
	shell_print(sh, "Stored %s, %u bytes", name, (unsigned int)size);
	return end(0);
}

static int cmd_discard(const struct shell *sh, size_t argc, char **argv)
{
	char part[PATH_MAX_LEN + 1];
	int err;

	if (!check_name(sh, argv[1])) {
		return -EINVAL;
	}
	make_path(part, sizeof(part), argv[1], PART_SUFFIX);

	if (!begin(sh)) {
		return -ENODEV;
	}
	if (part_in_use(argv[1])) {
		shell_error(sh, "The upload of %s is still arriving", argv[1]);
		return end(-EBUSY);
	}
	err = exists(part + sizeof(MOUNT_PREFIX) - 1) ? fs_unlink(part) : 0;
	if (err) {
		shell_error(sh, "Could not discard the upload (err %d)", err);
	}
	return end(err);
}

static int cmd_rm(const struct shell *sh, size_t argc, char **argv)
{
	char path[PATH_MAX_LEN + 1];
	int err;

	if (!check_name(sh, argv[1])) {
		return -EINVAL;
	}
	make_path(path, sizeof(path), argv[1], "");

	if (!begin(sh)) {
		return -ENODEV;
	}
	if (transfer_uses(argv[1])) {
		shell_error(sh, "%s is being downloaded: try again when that is done", argv[1]);
		return end(-EBUSY);
	}
	err = exists(argv[1]) ? fs_unlink(path) : -ENOENT;
	if (err == -ENOENT) {
		shell_error(sh, "No file %s", argv[1]);
	} else if (err) {
		shell_error(sh, "Could not remove %s (err %d)", argv[1], err);
	}
	return end(err);
}

SHELL_STATIC_SUBCMD_SET_CREATE(files_cmds,
	SHELL_CMD_ARG(crc, NULL, "<name>  Show a file's size and CRC-32", cmd_crc, 2, 0),
	SHELL_CMD_ARG(commit, NULL, "<name> <bytes> <crc32>  Store the upload of a file, "
		      "if it has this size and CRC-32", cmd_commit, 4, 0),
	SHELL_CMD_ARG(discard, NULL, "<name>  Drop an upload that was not stored",
		      cmd_discard, 2, 0),
	SHELL_CMD_ARG(rm, NULL, "<name>  Remove a file", cmd_rm, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_ARG_REGISTER(files, &files_cmds, "List the files, with their sizes and the space left",
		       cmd_files, 1, 0);

/* ------------------------------------------------------------------- init --- */

/* Removes what an upload or a write that never finished left behind. */
static void remove_leftovers(void)
{
	char path[sizeof(MOUNT_PREFIX) + MAX_FILE_NAME];
	struct fs_dirent entry;
	struct fs_dir_t dir;
	bool again;

	/* Removing changes the directory being read: start over after each. */
	do {
		again = false;
		fs_dir_t_init(&dir);
		if (fs_opendir(&dir, MOUNT_POINT) != 0) {
			return;
		}
		while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
			if (entry.type == FS_DIR_ENTRY_FILE &&
			    (has_suffix(entry.name, PART_SUFFIX) ||
			     has_suffix(entry.name, TMP_SUFFIX))) {
				snprintf(path, sizeof(path), MOUNT_PREFIX "%s", entry.name);
				again = true;
				break;
			}
		}
		(void)fs_closedir(&dir);
		if (again && fs_unlink(path) != 0) {
			return;
		}
	} while (again);
}

int files_init(void)
{
	int err;

	/* Refused, until there is a file system, by "mounted" being false. */
	mgmt_callback_register(&file_access_callback);
	mgmt_callback_register(&command_done_callback);

	k_mutex_lock(&lock, K_FOREVER);
	/* A flash that holds no file system yet is formatted. */
	err = fs_mount(&mount);
	if (err) {
		LOG_ERR("No file system on the QSPI flash (err %d)", err);
	} else {
		remove_leftovers();
		mounted = true;
	}
	k_mutex_unlock(&lock);

	return err;
}
