/*
 * SPDX-License-Identifier: MIT
 *
 * Bluetooth HID (HID over GATT) with five input reports, in two HID services
 * because a service's report map is limited to 512 bytes: mouse and keyboard
 * in one, the three sensors in the other.
 *
 *   1  mouse          5 buttons, relative X/Y (16 bit), wheel
 *   2  keyboard       modifiers and six keys, plus the LED output report
 *   3  accelerometer  HID Sensor "Accelerometer 3D", X/Y/Z in mg
 *   4  gyroscope      HID Sensor "Gyrometer 3D", X/Y/Z in 0.1 deg/s
 *   5  orientation    HID Sensor "Device Orientation", quaternion x/y/z/w
 *
 * Every report is off until it is enabled with "hid enable"; the choice is
 * kept across resets. The mouse and keyboard reports are driven from the "hid"
 * shell command. The enabled sensor reports are streamed to every connected
 * host: the IMU is sampled at 100 Hz, accelerometer and gyroscope are fused
 * into an orientation, and the reports are sent at the rate set with
 * "hid rate". Each sensor also has the HID Sensor property (feature) report
 * that operating system sensor drivers use to start, stop and pace it.
 *
 * The orientation is also available outside HID, as a notification on a small
 * service of its own, which is what the web app's orientation viewer reads.
 *
 * With "hid air on" the board is also an air mouse: turning it moves the
 * pointer. The gyroscope's turn rates are sent as mouse movement, 100 times a
 * second.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <bluetooth/services/hids.h>

#include "app.h"

LOG_MODULE_REGISTER(hid, LOG_LEVEL_INF);

/* Report IDs, as in the report map. */
#define REPORT_ID_MOUSE       1
#define REPORT_ID_KEYBOARD    2
#define REPORT_ID_ACCEL       3
#define REPORT_ID_GYRO        4
#define REPORT_ID_ORIENTATION 5

/* Order in which the input reports are registered with the service. */
enum {
	INPUT_MOUSE,
	INPUT_KEYBOARD,
	INPUT_ACCEL,
	INPUT_GYRO,
	INPUT_ORIENTATION,
	INPUT_COUNT,
};

#define MOUSE_REPORT_LEN       6 /* buttons, X (2), Y (2), wheel */
#define KEYBOARD_REPORT_LEN    8 /* modifiers, reserved, 6 keys */
#define KEYBOARD_LED_LEN       1
#define SENSOR3_REPORT_LEN     8 /* state, event, 3 x int16 */
#define ORIENTATION_REPORT_LEN 10 /* state, event, 4 x int16 */

/*
 * Sensor property (feature) report: connection type, reporting state, power
 * state, sensor state (1 byte each), report interval in ms (4), then the
 * change sensitivity of the data field (2).
 */
#define SENSOR_FEATURE_LEN 10

#define FEATURE_CONNECTION_TYPE 0
#define FEATURE_REPORTING_STATE 1
#define FEATURE_POWER_STATE     2
#define FEATURE_SENSOR_STATE    3
#define FEATURE_REPORT_INTERVAL 4
#define FEATURE_SENSITIVITY     8

/* Selector values, as indices into the lists in the report map. */
#define CONNECTION_PC_EXTERNAL     2
#define REPORTING_NO_EVENTS        0
#define REPORTING_ALL_EVENTS       1
#define REPORTING_NO_EVENTS_WAKE   3
#define POWER_D0_FULL              1
#define POWER_D4_OFF               5
#define SENSOR_STATE_NOT_AVAILABLE 2

/* HID Sensor "state" and "event" selector values used in the reports. */
#define SENSOR_STATE_READY        1
#define SENSOR_EVENT_DATA_UPDATED 3

#define SAMPLE_HZ         100
#define SENSOR_RATE_MIN   1
#define SENSOR_RATE_MAX   SAMPLE_HZ
#define SENSOR_RATE_DEFAULT 50

/* Pull of the accelerometer on the gyro-integrated orientation, per second. */
#define FUSION_GAIN 1.0f

#define KEY_TAP_MS 15

/*
 * The selector lists (connection type, reporting state, power state, sensor
 * state, sensor event) are given as usage ranges to keep the map small.
 *
 * Sensor properties, as a feature report. data_usage is the low byte of the
 * sensor's data field usage (0x04xx), which the change sensitivity refers to,
 * in the same units (exponent) as the data.
 */
#define SENSOR_PROPERTIES(data_usage, exponent)                                     \
	0x0A, 0x09, 0x03,       /* Usage (Property: Sensor Connection Type) */     \
	0x15, 0x00,             /* Logical Minimum (0) */                          \
	0x25, 0x02,             /* Logical Maximum (2) */                          \
	0x75, 0x08,             /* Report Size (8) */                              \
	0x95, 0x01,             /* Report Count (1) */                             \
	0xA1, 0x02,             /* Collection (Logical) */                         \
	0x1A, 0x30, 0x08,       /*   Usage Minimum (Connection: PC Integrated) */  \
	0x2A, 0x32, 0x08,       /*   Usage Maximum (Connection: PC External) */    \
	0xB1, 0x00,             /*   Feature (Data, Array, Absolute) */            \
	0xC0,                   /* End Collection */                               \
	0x0A, 0x16, 0x03,       /* Usage (Property: Reporting State) */            \
	0x25, 0x05,             /* Logical Maximum (5) */                          \
	0xA1, 0x02,             /* Collection (Logical) */                         \
	0x1A, 0x40, 0x08,       /*   Usage Minimum (Reporting: No Events) */       \
	0x2A, 0x45, 0x08,       /*   Usage Maximum (Reporting: Threshold, Wake) */ \
	0xB1, 0x00,             /*   Feature (Data, Array, Absolute) */            \
	0xC0,                   /* End Collection */                               \
	0x0A, 0x19, 0x03,       /* Usage (Property: Power State) */                \
	0x25, 0x05,             /* Logical Maximum (5) */                          \
	0xA1, 0x02,             /* Collection (Logical) */                         \
	0x1A, 0x50, 0x08,       /*   Usage Minimum (Power: Undefined) */           \
	0x2A, 0x55, 0x08,       /*   Usage Maximum (Power: D4 Power Off) */        \
	0xB1, 0x00,             /*   Feature (Data, Array, Absolute) */            \
	0xC0,                   /* End Collection */                               \
	0x0A, 0x01, 0x02,       /* Usage (Sensor State) */                         \
	0x25, 0x06,             /* Logical Maximum (6) */                          \
	0xA1, 0x02,             /* Collection (Logical) */                         \
	0x1A, 0x00, 0x08,       /*   Usage Minimum (State: Undefined) */           \
	0x2A, 0x06, 0x08,       /*   Usage Maximum (State: Error) */               \
	0xB1, 0x00,             /*   Feature (Data, Array, Absolute) */            \
	0xC0,                   /* End Collection */                               \
	0x0A, 0x0E, 0x03,       /* Usage (Property: Report Interval), in ms */     \
	0x27, 0xFF, 0xFF, 0xFF, 0x7F, /* Logical Maximum (2147483647) */           \
	0x75, 0x20,             /* Report Size (32) */                             \
	0x55, 0x00,             /* Unit Exponent (0) */                            \
	0xB1, 0x02,             /* Feature (Data, Variable, Absolute) */           \
	0x0A, data_usage, 0x14, /* Usage (data field: Change Sensitivity, absolute) */ \
	0x26, 0xFF, 0x7F,       /* Logical Maximum (32767) */                      \
	0x75, 0x10,             /* Report Size (16) */                             \
	0x55, exponent,         /* Unit Exponent: as the data field */             \
	0xB1, 0x02              /* Feature (Data, Variable, Absolute) */

/* The state and event bytes that start every sensor input report. */
#define SENSOR_STATE_AND_EVENT                                                      \
	0x0A, 0x01, 0x02,       /* Usage (Sensor State) */                         \
	0x25, 0x06,             /* Logical Maximum (6) */                          \
	0x75, 0x08,             /* Report Size (8) */                              \
	0xA1, 0x02,             /* Collection (Logical) */                         \
	0x1A, 0x00, 0x08,       /*   Usage Minimum (State: Undefined) */           \
	0x2A, 0x06, 0x08,       /*   Usage Maximum (State: Error) */               \
	0x81, 0x00,             /*   Input (Data, Array, Absolute) */              \
	0xC0,                   /* End Collection */                               \
	0x0A, 0x02, 0x02,       /* Usage (Sensor Event) */                         \
	0x25, 0x05,             /* Logical Maximum (5) */                          \
	0xA1, 0x02,             /* Collection (Logical) */                         \
	0x1A, 0x10, 0x08,       /*   Usage Minimum (Event: Unknown) */             \
	0x2A, 0x15, 0x08,       /*   Usage Maximum (Event: Change Sensitivity) */  \
	0x81, 0x00,             /*   Input (Data, Array, Absolute) */              \
	0xC0                    /* End Collection */

/* Report map of the mouse and keyboard service. */
static const uint8_t pointer_report_map[] = {
	/* ------------------------------------------------------- Mouse --- */
	0x05, 0x01,             /* Usage Page (Generic Desktop) */
	0x09, 0x02,             /* Usage (Mouse) */
	0xA1, 0x01,             /* Collection (Application) */
	0x85, REPORT_ID_MOUSE,  /*   Report ID */
	0x09, 0x01,             /*   Usage (Pointer) */
	0xA1, 0x00,             /*   Collection (Physical) */
	0x05, 0x09,             /*     Usage Page (Button) */
	0x19, 0x01,             /*     Usage Minimum (1) */
	0x29, 0x05,             /*     Usage Maximum (5) */
	0x15, 0x00,             /*     Logical Minimum (0) */
	0x25, 0x01,             /*     Logical Maximum (1) */
	0x75, 0x01,             /*     Report Size (1) */
	0x95, 0x05,             /*     Report Count (5) */
	0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
	0x75, 0x03,             /*     Report Size (3) */
	0x95, 0x01,             /*     Report Count (1) */
	0x81, 0x01,             /*     Input (Constant) */
	0x05, 0x01,             /*     Usage Page (Generic Desktop) */
	0x09, 0x30,             /*     Usage (X) */
	0x09, 0x31,             /*     Usage (Y) */
	0x16, 0x01, 0x80,       /*     Logical Minimum (-32767) */
	0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */
	0x75, 0x10,             /*     Report Size (16) */
	0x95, 0x02,             /*     Report Count (2) */
	0x81, 0x06,             /*     Input (Data, Variable, Relative) */
	0x09, 0x38,             /*     Usage (Wheel) */
	0x15, 0x81,             /*     Logical Minimum (-127) */
	0x25, 0x7F,             /*     Logical Maximum (127) */
	0x75, 0x08,             /*     Report Size (8) */
	0x95, 0x01,             /*     Report Count (1) */
	0x81, 0x06,             /*     Input (Data, Variable, Relative) */
	0xC0,                   /*   End Collection */
	0xC0,                   /* End Collection */

	/* ---------------------------------------------------- Keyboard --- */
	0x05, 0x01,             /* Usage Page (Generic Desktop) */
	0x09, 0x06,             /* Usage (Keyboard) */
	0xA1, 0x01,             /* Collection (Application) */
	0x85, REPORT_ID_KEYBOARD, /* Report ID */
	0x05, 0x07,             /*   Usage Page (Keyboard) */
	0x19, 0xE0,             /*   Usage Minimum (Left Control) */
	0x29, 0xE7,             /*   Usage Maximum (Right GUI) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x25, 0x01,             /*   Logical Maximum (1) */
	0x75, 0x01,             /*   Report Size (1) */
	0x95, 0x08,             /*   Report Count (8) */
	0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
	0x75, 0x08,             /*   Report Size (8) */
	0x95, 0x01,             /*   Report Count (1) */
	0x81, 0x01,             /*   Input (Constant) */
	0x05, 0x08,             /*   Usage Page (LEDs) */
	0x19, 0x01,             /*   Usage Minimum (Num Lock) */
	0x29, 0x05,             /*   Usage Maximum (Kana) */
	0x75, 0x01,             /*   Report Size (1) */
	0x95, 0x05,             /*   Report Count (5) */
	0x91, 0x02,             /*   Output (Data, Variable, Absolute) */
	0x75, 0x03,             /*   Report Size (3) */
	0x95, 0x01,             /*   Report Count (1) */
	0x91, 0x01,             /*   Output (Constant) */
	0x05, 0x07,             /*   Usage Page (Keyboard) */
	0x19, 0x00,             /*   Usage Minimum (0) */
	0x29, 0x65,             /*   Usage Maximum (101) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x25, 0x65,             /*   Logical Maximum (101) */
	0x75, 0x08,             /*   Report Size (8) */
	0x95, 0x06,             /*   Report Count (6) */
	0x81, 0x00,             /*   Input (Data, Array) */
	0xC0,                   /* End Collection */
};

/* Report map of the sensor service. */
static const uint8_t sensor_report_map[] = {
	0x05, 0x20,             /* Usage Page (Sensor) */
	0x09, 0x01,             /* Usage (Sensor: collection of sensors) */
	0xA1, 0x01,             /* Collection (Application) */

	0x85, REPORT_ID_ACCEL,  /*   Report ID */
	0x09, 0x73,             /*   Usage (Motion: Accelerometer 3D) */
	0xA1, 0x00,             /*   Collection (Physical) */
	SENSOR_PROPERTIES(0x52, 0x0D), /* data field: Acceleration, in mg */
	SENSOR_STATE_AND_EVENT,
	0x0A, 0x53, 0x04,       /*     Usage (Acceleration Axis X) */
	0x0A, 0x54, 0x04,       /*     Usage (Acceleration Axis Y) */
	0x0A, 0x55, 0x04,       /*     Usage (Acceleration Axis Z) */
	0x16, 0x01, 0x80,       /*     Logical Minimum (-32767) */
	0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */
	0x75, 0x10,             /*     Report Size (16) */
	0x95, 0x03,             /*     Report Count (3) */
	0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
	0xC0,                   /*   End Collection */

	0x85, REPORT_ID_GYRO,   /*   Report ID */
	0x09, 0x76,             /*   Usage (Motion: Gyrometer 3D) */
	0xA1, 0x00,             /*   Collection (Physical) */
	SENSOR_PROPERTIES(0x56, 0x0F), /* data field: Angular Velocity, 0.1 deg/s */
	SENSOR_STATE_AND_EVENT,
	0x0A, 0x57, 0x04,       /*     Usage (Angular Velocity X) */
	0x0A, 0x58, 0x04,       /*     Usage (Angular Velocity Y) */
	0x0A, 0x59, 0x04,       /*     Usage (Angular Velocity Z) */
	0x16, 0x01, 0x80,       /*     Logical Minimum (-32767) */
	0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */
	0x75, 0x10,             /*     Report Size (16) */
	0x95, 0x03,             /*     Report Count (3) */
	0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
	0xC0,                   /*   End Collection */

	0x85, REPORT_ID_ORIENTATION, /* Report ID */
	0x09, 0x8A,             /*   Usage (Orientation: Device Orientation) */
	0xA1, 0x00,             /*   Collection (Physical) */
	SENSOR_PROPERTIES(0x83, 0x0C), /* data field: Quaternion, 1/10000 */
	SENSOR_STATE_AND_EVENT,
	0x0A, 0x83, 0x04,       /*     Usage (Orientation: Quaternion) */
	0x16, 0xF0, 0xD8,       /*     Logical Minimum (-10000) */
	0x26, 0x10, 0x27,       /*     Logical Maximum (10000) */
	0x75, 0x10,             /*     Report Size (16) */
	0x95, 0x04,             /*     Report Count (4): x, y, z, w */
	0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
	0xC0,                   /*   End Collection */

	0xC0,                   /* End Collection */
};

BUILD_ASSERT(sizeof(pointer_report_map) <= 512 && sizeof(sensor_report_map) <= 512,
	     "A HID over GATT report map cannot exceed 512 bytes");

/* Bytes of the mouse report that are relative (movement and wheel). */
static const uint8_t mouse_relative_mask[] = { 0x3E };

BT_HIDS_DEF(pointer_hids, MOUSE_REPORT_LEN, KEYBOARD_REPORT_LEN, KEYBOARD_LED_LEN);
BT_HIDS_DEF(sensor_hids, SENSOR3_REPORT_LEN, SENSOR3_REPORT_LEN, ORIENTATION_REPORT_LEN,
	    SENSOR_FEATURE_LEN, SENSOR_FEATURE_LEN, SENSOR_FEATURE_LEN);

/* Which service each input report belongs to, and its position there. */
static const struct {
	struct bt_hids *service;
	uint8_t index;
} report_route[INPUT_COUNT] = {
	[INPUT_MOUSE] = { &pointer_hids, 0 },
	[INPUT_KEYBOARD] = { &pointer_hids, 1 },
	[INPUT_ACCEL] = { &sensor_hids, 0 },
	[INPUT_GYRO] = { &sensor_hids, 1 },
	[INPUT_ORIENTATION] = { &sensor_hids, 2 },
};

static const char *const report_names[INPUT_COUNT] = {
	"mouse", "keyboard", "accel", "gyro", "orientation",
};

#define SENSOR_REPORTS (BIT(INPUT_ACCEL) | BIT(INPUT_GYRO) | BIT(INPUT_ORIENTATION))
#define ALL_REPORTS    BIT_MASK(INPUT_COUNT)

/*
 * The settings are kept for each paired device (peer) separately: which
 * reports it is sent, how often the sensor ones, and how the air mouse behaves
 * for it. One host may want a mouse and another only the orientation.
 *
 * A peer without settings of its own has the defaults, which on a board
 * upgraded from firmware with one set of settings for all are those settings.
 */
struct air_config {
	uint8_t on;
	uint8_t speed;    /* pointer X counts per degree turned */
	uint8_t deadzone; /* turn rates below this many deg/s are ignored */
	int8_t axis_x;    /* manual axes */
	int8_t axis_y;
	uint8_t manual;   /* 0: automatic axes */
	int8_t forward;   /* the board axis that points at the screen */
	uint8_t speed_y;  /* pointer Y counts per degree turned */
	uint8_t accel;    /* turn rate (deg/s) at which the speeds apply; 0: no acceleration */
} __packed;

struct hid_profile {
	uint8_t enabled; /* reports switched on with "hid enable", bit per INPUT_ index */
	uint8_t rate;    /* sensor reports per second */
	struct air_config air;
} __packed;

/* A peer's own settings. Stored as it is, under "hid/peer/<slot>". */
struct peer_profile {
	bt_addr_le_t addr;
	uint8_t used;
	struct hid_profile profile;
} __packed;

static struct hid_profile defaults = {
	.enabled = 0,
	.rate = SENSOR_RATE_DEFAULT,
	.air = { .on = 0, .speed = 20, .deadzone = 2, .axis_x = -3, .axis_y = -1,
		 .manual = 0, .forward = 2, .speed_y = 20, .accel = 60 },
};

static struct peer_profile peers[CONFIG_BT_MAX_PAIRED];

static struct peer_profile *peer_find(const bt_addr_le_t *addr)
{
	ARRAY_FOR_EACH_PTR(peers, peer) {
		if (peer->used && bt_addr_le_eq(&peer->addr, addr)) {
			return peer;
		}
	}
	return NULL;
}

/* The settings that apply to a peer: its own, or the defaults. */
static const struct hid_profile *profile_of(const bt_addr_le_t *addr)
{
	const struct peer_profile *peer = peer_find(addr);

	return peer != NULL ? &peer->profile : &defaults;
}

static const struct hid_profile *profile_for(struct bt_conn *conn)
{
	return profile_of(bt_conn_get_dst(conn));
}

/*
 * What a host has asked of each sensor through its property report. A sensor
 * report is sent only if it is enabled here and the host has not turned its
 * events or power off.
 */
static struct {
	uint8_t reporting_state;
	uint8_t power_state;
} host_request[INPUT_COUNT] = {
	[INPUT_ACCEL] = { REPORTING_ALL_EVENTS, POWER_D0_FULL },
	[INPUT_GYRO] = { REPORTING_ALL_EVENTS, POWER_D0_FULL },
	[INPUT_ORIENTATION] = { REPORTING_ALL_EVENTS, POWER_D0_FULL },
};

/* Input reports that at least one host is currently subscribed to (bit per report). */
static atomic_t subscribed;
static atomic_t host_count;
/* A report interval a host asked for in a property report; 0 if none has. */
static atomic_t host_rate_hz;

static bool report_enabled(const struct hid_profile *profile, int index)
{
	return (profile->enabled & BIT(index)) != 0;
}

static int sensor_rate(const struct hid_profile *profile)
{
	int asked = atomic_get(&host_rate_hz);

	return asked != 0 ? asked : profile->rate;
}

/* A sensor report goes out if it is enabled and the host wants its events. */
static bool sensor_wanted(const struct hid_profile *profile, int index)
{
	uint8_t reporting = host_request[index].reporting_state;

	return report_enabled(profile, index) &&
	       host_request[index].power_state != POWER_D4_OFF &&
	       reporting != REPORTING_NO_EVENTS && reporting != REPORTING_NO_EVENTS_WAKE;
}
static uint8_t keyboard_leds;

/*
 * Air mouse: turning the board moves the pointer. Board axes are stored as
 * 1, 2, 3 for x, y, z and negative for the opposite direction.
 *
 * Automatic axes work the directions out from gravity, however the board is
 * held: turning about the vertical moves the pointer sideways, and tipping the
 * board's "forward" axis up or down moves it up or down. Manual axes tie each
 * pointer axis to one gyroscope axis instead.
 */
#define AIR_SPEED_MIN     1
#define AIR_SPEED_MAX     200
#define AIR_DEADZONE_MAX  20
#define AIR_ACCEL_MAX     250

static bool air_config_valid(const struct air_config *config)
{
	return config->on <= 1 &&
	       config->speed >= AIR_SPEED_MIN && config->speed <= AIR_SPEED_MAX &&
	       config->speed_y >= AIR_SPEED_MIN && config->speed_y <= AIR_SPEED_MAX &&
	       config->deadzone <= AIR_DEADZONE_MAX &&
	       abs(config->axis_x) >= 1 && abs(config->axis_x) <= 3 &&
	       abs(config->axis_y) >= 1 && abs(config->axis_y) <= 3 &&
	       config->manual <= 1 &&
	       abs(config->forward) >= 1 && abs(config->forward) <= 3;
}

static bool air_active(const struct hid_profile *profile)
{
	return profile->air.on && report_enabled(profile, INPUT_MOUSE);
}

/* ------------------------------------------------------------- sending --- */

/* Sends an input report to one host. Fails if it has not subscribed to it. */
static int send_to(struct bt_conn *conn, uint8_t index, const uint8_t *data, uint8_t len)
{
	return bt_hids_inp_rep_send(report_route[index].service, conn, report_route[index].index,
				    data, len, NULL);
}

/*
 * The peer that "hid" commands are about: the one chosen with "hid peer", or
 * the only one there is. See selected_peer().
 */
static bt_addr_le_t selected;
static bool have_selected;

/*
 * Sends an input report to the selected peer, for the commands that act as a
 * mouse or keyboard. Returns 1 if it took it, 0 if not (or not connected).
 */
static int send_report(uint8_t index, const uint8_t *data, uint8_t len)
{
	struct bt_conn *conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &selected);
	int sent = 0;

	if (conn != NULL) {
		sent = send_to(conn, index, data, len) == 0;
		bt_conn_unref(conn);
	}
	return sent;
}

int hid_connected(struct bt_conn *conn)
{
	int err = bt_hids_connected(&pointer_hids, conn);

	if (!err) {
		err = bt_hids_connected(&sensor_hids, conn);
		if (err) {
			bt_hids_disconnected(&pointer_hids, conn);
		}
	}
	if (!err) {
		atomic_inc(&host_count);
	}
	return err;
}

void hid_disconnected(struct bt_conn *conn)
{
	bt_hids_disconnected(&sensor_hids, conn);
	if (bt_hids_disconnected(&pointer_hids, conn) == 0) {
		atomic_dec(&host_count);
	}
}

static void on_subscription(uint8_t report_id, enum bt_hids_notify_evt evt)
{
	if (report_id < REPORT_ID_MOUSE || report_id > REPORT_ID_ORIENTATION) {
		return;
	}
	if (evt == BT_HIDS_CCCD_EVT_NOTIFY_ENABLED) {
		atomic_set_bit(&subscribed, report_id);
	} else {
		atomic_clear_bit(&subscribed, report_id);
	}
	LOG_DBG("Report %u notifications %s", report_id,
		evt == BT_HIDS_CCCD_EVT_NOTIFY_ENABLED ? "on" : "off");
}

static void on_keyboard_leds(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	if (write && rep->size >= 1) {
		keyboard_leds = rep->data[0];
	}
}

/* Reads or writes the property report of one sensor. */
static void sensor_feature(int index, struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	const struct hid_profile *profile = conn != NULL ? profile_for(conn) : &defaults;
	uint8_t *data = rep->data;

	if (rep->size < SENSOR_FEATURE_LEN) {
		return;
	}

	if (write) {
		uint32_t interval_ms = sys_get_le32(&data[FEATURE_REPORT_INTERVAL]);

		host_request[index].reporting_state = data[FEATURE_REPORTING_STATE];
		host_request[index].power_state = data[FEATURE_POWER_STATE];

		/* One rate serves all three sensors; zero means "your default". */
		if (interval_ms != 0) {
			atomic_set(&host_rate_hz,
				   CLAMP(1000 / interval_ms, SENSOR_RATE_MIN, SENSOR_RATE_MAX));
		}
		return;
	}

	memset(data, 0, SENSOR_FEATURE_LEN);
	data[FEATURE_CONNECTION_TYPE] = CONNECTION_PC_EXTERNAL;
	data[FEATURE_REPORTING_STATE] = host_request[index].reporting_state;
	data[FEATURE_POWER_STATE] = host_request[index].power_state;
	data[FEATURE_SENSOR_STATE] = report_enabled(profile, index) ? SENSOR_STATE_READY
								   : SENSOR_STATE_NOT_AVAILABLE;
	sys_put_le32(1000 / sensor_rate(profile), &data[FEATURE_REPORT_INTERVAL]);
	sys_put_le16(0, &data[FEATURE_SENSITIVITY]);
}

static void on_accel_feature(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	sensor_feature(INPUT_ACCEL, rep, conn, write);
}

static void on_gyro_feature(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	sensor_feature(INPUT_GYRO, rep, conn, write);
}

static void on_orientation_feature(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	sensor_feature(INPUT_ORIENTATION, rep, conn, write);
}

static int peers_init(void);

int hid_init(void)
{
	struct bt_hids_init_param pointer = { 0 };
	struct bt_hids_init_param sensors = { 0 };
	struct bt_hids_inp_rep *in;
	int err;

	pointer.rep_map.data = pointer_report_map;
	pointer.rep_map.size = sizeof(pointer_report_map);
	pointer.info.bcd_hid = 0x0111;
	pointer.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;

	in = pointer.inp_rep_group_init.reports;
	in[0].id = REPORT_ID_MOUSE;
	in[0].size = MOUSE_REPORT_LEN;
	in[0].rep_mask = mouse_relative_mask;
	in[0].handler_ext = on_subscription;
	in[1].id = REPORT_ID_KEYBOARD;
	in[1].size = KEYBOARD_REPORT_LEN;
	in[1].handler_ext = on_subscription;
	pointer.inp_rep_group_init.cnt = 2;

	pointer.outp_rep_group_init.reports[0].id = REPORT_ID_KEYBOARD;
	pointer.outp_rep_group_init.reports[0].size = KEYBOARD_LED_LEN;
	pointer.outp_rep_group_init.reports[0].handler = on_keyboard_leds;
	pointer.outp_rep_group_init.cnt = 1;

	err = peers_init();
	if (err) {
		return err;
	}

	err = bt_hids_init(&pointer_hids, &pointer);
	if (err) {
		return err;
	}

	sensors.rep_map.data = sensor_report_map;
	sensors.rep_map.size = sizeof(sensor_report_map);
	sensors.info.bcd_hid = 0x0111;
	sensors.info.flags = BT_HIDS_NORMALLY_CONNECTABLE;

	in = sensors.inp_rep_group_init.reports;
	in[0].id = REPORT_ID_ACCEL;
	in[0].size = SENSOR3_REPORT_LEN;
	in[1].id = REPORT_ID_GYRO;
	in[1].size = SENSOR3_REPORT_LEN;
	in[2].id = REPORT_ID_ORIENTATION;
	in[2].size = ORIENTATION_REPORT_LEN;
	for (int i = 0; i < 3; i++) {
		in[i].handler_ext = on_subscription;
		sensors.feat_rep_group_init.reports[i].id = in[i].id;
		sensors.feat_rep_group_init.reports[i].size = SENSOR_FEATURE_LEN;
	}
	sensors.inp_rep_group_init.cnt = 3;

	sensors.feat_rep_group_init.reports[0].handler = on_accel_feature;
	sensors.feat_rep_group_init.reports[1].handler = on_gyro_feature;
	sensors.feat_rep_group_init.reports[2].handler = on_orientation_feature;
	sensors.feat_rep_group_init.cnt = 3;

	return bt_hids_init(&sensor_hids, &sensors);
}

/* -------------------------------------------------------- sensor fusion --- */

/* Orientation of the board relative to where it was when streaming started. */
static struct {
	float w, x, y, z;
} q = { 1.0f, 0.0f, 0.0f, 0.0f };

static void fusion_reset(void)
{
	q.w = 1.0f;
	q.x = q.y = q.z = 0.0f;
}

/*
 * Complementary filter: integrate the gyroscope, and steer the result so that
 * "down" in the estimate lines up with the measured gravity. Without a
 * magnetometer nothing corrects the heading (rotation about vertical), so it
 * drifts slowly even with the gyroscope's zero offset removed.
 *
 * Gyroscope in rad/s, accelerometer in any unit, dt in seconds.
 */
static void fusion_update(float gx, float gy, float gz, float ax, float ay, float az, float dt)
{
	float norm = sqrtf(ax * ax + ay * ay + az * az);

	if (norm > 0.0f) {
		/* Direction of gravity according to the current estimate. */
		float vx = 2.0f * (q.x * q.z - q.w * q.y);
		float vy = 2.0f * (q.w * q.x + q.y * q.z);
		float vz = q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z;

		ax /= norm;
		ay /= norm;
		az /= norm;

		/* The cross product is the rotation that would align the two. */
		gx += FUSION_GAIN * (ay * vz - az * vy);
		gy += FUSION_GAIN * (az * vx - ax * vz);
		gz += FUSION_GAIN * (ax * vy - ay * vx);
	}

	float half_dt = 0.5f * dt;
	float dw = (-q.x * gx - q.y * gy - q.z * gz) * half_dt;
	float dx = (q.w * gx + q.y * gz - q.z * gy) * half_dt;
	float dy = (q.w * gy - q.x * gz + q.z * gx) * half_dt;
	float dz = (q.w * gz + q.x * gy - q.y * gx) * half_dt;

	q.w += dw;
	q.x += dx;
	q.y += dy;
	q.z += dz;

	norm = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	if (norm > 0.0f) {
		q.w /= norm;
		q.x /= norm;
		q.y /= norm;
		q.z /= norm;
	} else {
		fusion_reset();
	}
}

/*
 * Gyroscope zero offset, in deg/s. Without correcting it the orientation turns
 * steadily even when the board is still. It is measured whenever the board
 * has been at rest for a second: both sensors steady over the whole second.
 */
#define REST_GYRO_SPREAD_DPS 1.5f
#define REST_ACCEL_SPREAD_G  0.03f

static float gyro_bias[3];

static struct {
	float gyro_min[3], gyro_max[3], gyro_sum[3];
	float accel_min[3], accel_max[3];
	int count;
} rest;

static void bias_reset_window(void)
{
	for (int i = 0; i < 3; i++) {
		rest.gyro_min[i] = rest.accel_min[i] = INFINITY;
		rest.gyro_max[i] = rest.accel_max[i] = -INFINITY;
		rest.gyro_sum[i] = 0.0f;
	}
	rest.count = 0;
}

static void bias_update(const float accel_g[3], const float gyro_dps[3])
{
	bool at_rest = true;

	for (int i = 0; i < 3; i++) {
		rest.gyro_min[i] = MIN(rest.gyro_min[i], gyro_dps[i]);
		rest.gyro_max[i] = MAX(rest.gyro_max[i], gyro_dps[i]);
		rest.gyro_sum[i] += gyro_dps[i];
		rest.accel_min[i] = MIN(rest.accel_min[i], accel_g[i]);
		rest.accel_max[i] = MAX(rest.accel_max[i], accel_g[i]);
	}

	if (++rest.count < SAMPLE_HZ) {
		return;
	}

	for (int i = 0; i < 3; i++) {
		if (rest.gyro_max[i] - rest.gyro_min[i] > REST_GYRO_SPREAD_DPS ||
		    rest.accel_max[i] - rest.accel_min[i] > REST_ACCEL_SPREAD_G) {
			at_rest = false;
		}
	}
	if (at_rest) {
		for (int i = 0; i < 3; i++) {
			gyro_bias[i] = rest.gyro_sum[i] / rest.count;
		}
	}
	bias_reset_window();
}

static int16_t to_int16(float value)
{
	return (int16_t)CLAMP(lroundf(value), -32767, 32767);
}

/* Sends one host the sensor reports that are on for it. */
static void send_sensor_reports(struct bt_conn *conn, const struct hid_profile *profile,
				const float accel_g[3], const float gyro_dps[3])
{
	uint8_t report[ORIENTATION_REPORT_LEN] = { SENSOR_STATE_READY, SENSOR_EVENT_DATA_UPDATED };
	const float quaternion[4] = { q.x, q.y, q.z, q.w };

	if (sensor_wanted(profile, INPUT_ACCEL)) {
		for (int i = 0; i < 3; i++) {
			sys_put_le16(to_int16(accel_g[i] * 1000.0f), &report[2 + 2 * i]);
		}
		(void)send_to(conn, INPUT_ACCEL, report, SENSOR3_REPORT_LEN);
	}

	if (sensor_wanted(profile, INPUT_GYRO)) {
		for (int i = 0; i < 3; i++) {
			sys_put_le16(to_int16(gyro_dps[i] * 10.0f), &report[2 + 2 * i]);
		}
		(void)send_to(conn, INPUT_GYRO, report, SENSOR3_REPORT_LEN);
	}

	if (sensor_wanted(profile, INPUT_ORIENTATION)) {
		for (int i = 0; i < 4; i++) {
			sys_put_le16(to_int16(quaternion[i] * 10000.0f), &report[2 + 2 * i]);
		}
		(void)send_to(conn, INPUT_ORIENTATION, report, ORIENTATION_REPORT_LEN);
	}
}

/* ------------------------------------------------- orientation stream --- */

/*
 * The fused orientation for applications that are not HID hosts, such as the
 * web app's viewer: one characteristic that notifies the quaternion x, y, z,
 * w (board to room, each scaled by 10000, little endian) 50 times a second
 * while anybody is subscribed. Like the rest, only on an encrypted link.
 */
#define UUID_ORIENTATION_SERVICE BT_UUID_DECLARE_128( \
	BT_UUID_128_ENCODE(0x7c3b0001, 0x5f1e, 0x4c8a, 0x9d2e, 0x3a6b8f1c2d4e))
#define UUID_ORIENTATION_VALUE BT_UUID_DECLARE_128( \
	BT_UUID_128_ENCODE(0x7c3b0002, 0x5f1e, 0x4c8a, 0x9d2e, 0x3a6b8f1c2d4e))

#define ORIENTATION_STREAM_HZ 50

static atomic_t stream_wanted;

static void on_stream_subscription(const struct bt_gatt_attr *attr, uint16_t value)
{
	atomic_set(&stream_wanted, value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(orientation_service,
	BT_GATT_PRIMARY_SERVICE(UUID_ORIENTATION_SERVICE),
	BT_GATT_CHARACTERISTIC(UUID_ORIENTATION_VALUE, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(on_stream_subscription, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
);

static void send_orientation_stream(void)
{
	const float quaternion[4] = { q.x, q.y, q.z, q.w };
	uint8_t value[8];

	for (int i = 0; i < 4; i++) {
		sys_put_le16(to_int16(quaternion[i] * 10000.0f), &value[2 * i]);
	}
	/* To every subscriber. Dropped if the link is busy: the next one follows. */
	(void)bt_gatt_notify(NULL, &orientation_service.attrs[1], value, sizeof(value));
}

/* What is kept for each connection while the sensors are running. */
static struct {
	/* Movement not sent yet: the fractions of a count left over from each sample. */
	float air_pending[2];
	/* Counts up to the next sensor report, at this host's rate. */
	int since_report;
} link_state[CONFIG_BT_MAX_CONN];

static float air_rate(const float turn_dps[3], int8_t axis)
{
	float rate = turn_dps[abs(axis) - 1];

	return axis < 0 ? -rate : rate;
}

/*
 * Which way is up, as a unit vector in the board's own axes. The gyroscope
 * keeps it right while the board turns, and the accelerometer, which reads
 * straight up when the board is not being shaken, pulls it back slowly so
 * that it cannot drift.
 */
#define UP_CORRECTION_PER_S 2.0f
/* Forward closer to vertical than this (sine of the angle) leaves no "sideways". */
#define FORWARD_MIN_TILT    0.3f

static float up[3];
static bool up_known;

static void up_update(const float turn_dps[3], const float accel_g[3], float dt)
{
	const float deg_to_rad = 0.017453293f;
	float w[3] = { turn_dps[0] * deg_to_rad, turn_dps[1] * deg_to_rad,
		       turn_dps[2] * deg_to_rad };
	float accel = sqrtf(accel_g[0] * accel_g[0] + accel_g[1] * accel_g[1] +
			    accel_g[2] * accel_g[2]);
	float norm;

	if (!up_known) {
		if (accel < 0.5f) {
			return;
		}
		for (int i = 0; i < 3; i++) {
			up[i] = accel_g[i] / accel;
		}
		up_known = true;
		return;
	}

	/* A direction fixed in the room turns the opposite way in the board's axes. */
	float turned[3] = {
		up[0] - (w[1] * up[2] - w[2] * up[1]) * dt,
		up[1] - (w[2] * up[0] - w[0] * up[2]) * dt,
		up[2] - (w[0] * up[1] - w[1] * up[0]) * dt,
	};

	for (int i = 0; i < 3; i++) {
		/* Trust the accelerometer only while it reads about 1 g. */
		if (accel > 0.8f && accel < 1.2f) {
			turned[i] += (accel_g[i] / accel - turned[i]) * UP_CORRECTION_PER_S * dt;
		}
	}
	norm = sqrtf(turned[0] * turned[0] + turned[1] * turned[1] + turned[2] * turned[2]);
	if (norm > 0.0f) {
		for (int i = 0; i < 3; i++) {
			up[i] = turned[i] / norm;
		}
	}
}

/*
 * Pointer turn rates (deg/s, right and down positive) from gravity. Returns
 * false while they cannot be told: forward is pointing nearly straight up or
 * down, or up is not known yet.
 */
static bool air_auto_rates(const struct air_config *air, const float turn_dps[3], float rate[2])
{
	float forward[3] = { 0.0f, 0.0f, 0.0f };
	float right[3];
	float tilt;

	if (!up_known) {
		return false;
	}
	forward[abs(air->forward) - 1] = air->forward < 0 ? -1.0f : 1.0f;

	/* Forward x up points to the right, and is shorter the steeper forward is. */
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	tilt = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
	if (tilt < FORWARD_MIN_TILT) {
		return false;
	}

	/* Turning left (anticlockwise seen from above) moves the pointer left. */
	rate[0] = -(turn_dps[0] * up[0] + turn_dps[1] * up[1] + turn_dps[2] * up[2]);
	/* Turning about the right-pointing axis lifts forward: pointer up. */
	rate[1] = -(turn_dps[0] * right[0] + turn_dps[1] * right[1] +
		    turn_dps[2] * right[2]) / tilt;

	return true;
}

/*
 * Turns one sample's turn rates (deg/s, zero offset removed) into pointer
 * movement for one host, with that host's settings.
 */
static void air_update(struct bt_conn *conn, const struct air_config *air,
		       const float turn_dps[3], float dt)
{
	float *air_pending = link_state[bt_conn_index(conn)].air_pending;
	uint8_t report[MOUSE_REPORT_LEN] = { 0 };
	float rate[2];
	float speed;
	float gain;
	int16_t move[2];

	if (air->manual) {
		rate[0] = air_rate(turn_dps, air->axis_x);
		rate[1] = air_rate(turn_dps, air->axis_y);
	} else if (!air_auto_rates(air, turn_dps, rate)) {
		return;
	}
	speed = sqrtf(rate[0] * rate[0] + rate[1] * rate[1]);

	/*
	 * The dead zone keeps a hand's tremor and sensor noise from moving the
	 * pointer. Subtracting it, rather than cutting off below it, lets
	 * movement start from zero instead of with a jump.
	 */
	if (speed <= air->deadzone) {
		return;
	}
	/* Same direction as the turn, at the turn rate that is left above the dead zone. */
	gain = (speed - air->deadzone) / speed;
	/*
	 * Acceleration: the pointer speed grows with the square of the turn rate
	 * instead of in step with it, so slow turns are fine and precise and fast
	 * ones cross the screen. At the reference rate it is what it would be
	 * without acceleration; at half that rate a half, at twice that rate double.
	 */
	if (air->accel != 0) {
		gain *= (speed - air->deadzone) / air->accel;
	}
	for (int i = 0; i < 2; i++) {
		air_pending[i] += rate[i] * gain * dt * (i == 0 ? air->speed : air->speed_y);
		move[i] = to_int16(truncf(air_pending[i]));
	}
	if (move[0] == 0 && move[1] == 0) {
		return;
	}

	sys_put_le16(move[0], &report[1]);
	sys_put_le16(move[1], &report[3]);
	if (send_to(conn, INPUT_MOUSE, report, sizeof(report)) == 0) {
		air_pending[0] -= move[0];
		air_pending[1] -= move[1];
	} else {
		/* Nobody to move a pointer for: do not save the movement up. */
		air_pending[0] = air_pending[1] = 0.0f;
	}
}

/* Whether any connected host is to be sent sensor reports or pointer movement. */
static void check_wanted(struct bt_conn *conn, void *user_data)
{
	const struct hid_profile *profile = profile_for(conn);
	bool *wanted = user_data;

	if (air_active(profile) || sensor_wanted(profile, INPUT_ACCEL) ||
	    sensor_wanted(profile, INPUT_GYRO) || sensor_wanted(profile, INPUT_ORIENTATION)) {
		*wanted = true;
	}
}

struct sample {
	const float *accel_g;
	const float *gyro_dps;
	const float *turn_dps;
	float dt;
};

/* One sample's worth of reports for one host, by that host's settings. */
static void report_sample(struct bt_conn *conn, void *user_data)
{
	const struct hid_profile *profile = profile_for(conn);
	const struct sample *sample = user_data;
	int *since_report = &link_state[bt_conn_index(conn)].since_report;

	if (air_active(profile)) {
		air_update(conn, &profile->air, sample->turn_dps, sample->dt);
	}

	*since_report += sensor_rate(profile);
	if (*since_report >= SAMPLE_HZ) {
		*since_report -= SAMPLE_HZ;
		send_sensor_reports(conn, profile, sample->accel_g, sample->gyro_dps);
	}
}

static void sensor_thread(void *p1, void *p2, void *p3)
{
	const float deg_to_rad = 0.017453293f;
	int64_t next = k_uptime_get();
	int64_t last_sample = 0;
	bool streaming = false;
	int since_stream = 0;

	while (1) {
		float accel_g[3];
		float gyro_dps[3];
		bool wanted = atomic_get(&stream_wanted) != 0;

		if (atomic_get(&host_count) != 0 && !wanted) {
			bt_conn_foreach(BT_CONN_TYPE_LE, check_wanted, &wanted);
		}
		if (atomic_get(&host_count) == 0 || !wanted) {
			streaming = false;
			k_msleep(200);
			continue;
		}

		if (!streaming) {
			streaming = true;
			memset(link_state, 0, sizeof(link_state));
			next = k_uptime_get();
			last_sample = next - 1000 / SAMPLE_HZ;
			fusion_reset();
			bias_reset_window();
			up_known = false;
		}

		if (imu_sample(accel_g, gyro_dps) == 0) {
			int64_t now = k_uptime_get();
			/* Use the time that really passed: a slow link can delay the loop. */
			float dt = (now - last_sample) / 1000.0f;
			float turn_dps[3];

			bias_update(accel_g, gyro_dps);
			for (int i = 0; i < 3; i++) {
				turn_dps[i] = gyro_dps[i] - gyro_bias[i];
			}

			fusion_update(turn_dps[0] * deg_to_rad, turn_dps[1] * deg_to_rad,
				      turn_dps[2] * deg_to_rad,
				      accel_g[0], accel_g[1], accel_g[2], dt);
			last_sample = now;

			since_stream += ORIENTATION_STREAM_HZ;
			if (since_stream >= SAMPLE_HZ) {
				since_stream -= SAMPLE_HZ;
				if (atomic_get(&stream_wanted)) {
					send_orientation_stream();
				}
			}

			up_update(turn_dps, accel_g, dt);

			struct sample sample = {
				.accel_g = accel_g, .gyro_dps = gyro_dps, .turn_dps = turn_dps, .dt = dt,
			};

			bt_conn_foreach(BT_CONN_TYPE_LE, report_sample, &sample);
		}

		next += 1000 / SAMPLE_HZ;
		if (next < k_uptime_get()) {
			/* Fell behind (slow link): do not try to catch up. */
			next = k_uptime_get();
		}
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_DEFINE(hid_sensor_tid, 2048, sensor_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(8), 0, 0);

/* ------------------------------------------------------------ settings --- */

static int hid_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			    void *cb_arg)
{
	const char *slot;
	uint8_t value;
	ssize_t rc;

	if (settings_name_steq(name, "peer", &slot) && slot != NULL) {
		struct peer_profile stored = { .profile = defaults };
		int index = atoi(slot);

		if (index < 0 || index >= ARRAY_SIZE(peers) || len > sizeof(stored)) {
			return 0;
		}
		rc = read_cb(cb_arg, &stored, len);
		if (rc < 0) {
			return rc;
		}
		stored.profile.enabled &= ALL_REPORTS;
		if (stored.used == 1 && air_config_valid(&stored.profile.air) &&
		    stored.profile.rate >= SENSOR_RATE_MIN &&
		    stored.profile.rate <= SENSOR_RATE_MAX) {
			peers[index] = stored;
		}
		return 0;
	}

	/*
	 * The rest are the settings of firmware that had one set for every
	 * host. They are not written any more, and serve as the defaults.
	 */
	if (settings_name_steq(name, "air", NULL)) {
		/* Fields added since it was saved keep their defaults. */
		struct air_config stored = defaults.air;

		if (len > sizeof(stored)) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, &stored, len);
		if (rc < 0) {
			return rc;
		}
		if (len <= offsetof(struct air_config, speed_y)) {
			/* Saved when one speed served both directions. */
			stored.speed_y = stored.speed;
		}
		if (air_config_valid(&stored)) {
			defaults.air = stored;
		}
		return 0;
	}

	if (len != sizeof(value)) {
		return -EINVAL;
	}
	rc = read_cb(cb_arg, &value, sizeof(value));
	if (rc < 0) {
		return rc;
	}

	if (settings_name_steq(name, "enabled", NULL)) {
		defaults.enabled = value & ALL_REPORTS;
		return 0;
	}
	if (settings_name_steq(name, "rate", NULL)) {
		if (value >= SENSOR_RATE_MIN && value <= SENSOR_RATE_MAX) {
			defaults.rate = value;
		}
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(hid, "hid", NULL, hid_settings_set, NULL, NULL);

/* ------------------------------------------------------------ keyboard --- */

#define MOD_CTRL  BIT(0)
#define MOD_SHIFT BIT(1)
#define MOD_ALT   BIT(2)
#define MOD_GUI   BIT(3)

static const struct {
	const char *name;
	uint8_t usage;
} named_keys[] = {
	{ "enter", 0x28 },    { "esc", 0x29 },      { "backspace", 0x2a }, { "tab", 0x2b },
	{ "space", 0x2c },    { "capslock", 0x39 }, { "insert", 0x49 },    { "home", 0x4a },
	{ "pageup", 0x4b },   { "delete", 0x4c },   { "end", 0x4d },       { "pagedown", 0x4e },
	{ "right", 0x4f },    { "left", 0x50 },     { "down", 0x51 },      { "up", 0x52 },
	{ "f1", 0x3a },       { "f2", 0x3b },       { "f3", 0x3c },        { "f4", 0x3d },
	{ "f5", 0x3e },       { "f6", 0x3f },       { "f7", 0x40 },        { "f8", 0x41 },
	{ "f9", 0x42 },       { "f10", 0x43 },      { "f11", 0x44 },       { "f12", 0x45 },
};

static const struct {
	const char *name;
	uint8_t modifier;
} named_modifiers[] = {
	{ "ctrl", MOD_CTRL }, { "shift", MOD_SHIFT }, { "alt", MOD_ALT },
	{ "gui", MOD_GUI },   { "cmd", MOD_GUI },     { "win", MOD_GUI },
};

/* Key usage and shift state that produce an ASCII character on a US layout. */
static bool ascii_to_key(char c, uint8_t *usage, uint8_t *modifiers)
{
	static const char unshifted[] = "1234567890\n\x1b\b\t -=[]\\#;'`,./";
	/* \x01 marks positions that have no shifted character. */
	static const char shifted[] = "!@#$%^&*()\x01\x01\x01\x01\x01_+{}|\x01:\"~<>?";
	const char *found;

	*modifiers = 0;

	if (c >= 'a' && c <= 'z') {
		*usage = 0x04 + (c - 'a');
		return true;
	}
	if (c >= 'A' && c <= 'Z') {
		*usage = 0x04 + (c - 'A');
		*modifiers = MOD_SHIFT;
		return true;
	}
	if ((unsigned char)c < ' ' && c != '\n' && c != '\t') {
		return false;
	}

	found = memchr(unshifted, c, sizeof(unshifted) - 1);
	if (found != NULL && c != '#') {
		*usage = 0x1e + (found - unshifted);
		return true;
	}

	found = memchr(shifted, c, sizeof(shifted) - 1);
	if (found != NULL) {
		*usage = 0x1e + (found - shifted);
		*modifiers = MOD_SHIFT;
		return true;
	}

	return false;
}

/* Presses and releases one key. Returns false if no host took the report. */
static bool tap_key(uint8_t modifiers, uint8_t usage)
{
	uint8_t report[KEYBOARD_REPORT_LEN] = { modifiers, 0, usage };
	bool sent = send_report(INPUT_KEYBOARD, report, sizeof(report)) > 0;

	k_msleep(KEY_TAP_MS);
	memset(report, 0, sizeof(report));
	send_report(INPUT_KEYBOARD, report, sizeof(report));
	k_msleep(KEY_TAP_MS);

	return sent;
}

/* --------------------------------------------------------------- shell --- */

/*
 * The commands are about one peer, by its number in the "bonds" list:
 *
 *   hid peer <number>   chooses it; it stays chosen until another is
 *   hid peer            shows which it is
 *
 * With only one device paired there is nothing to choose.
 */
struct bond_lookup {
	int index;       /* counts the bonds, in the order "bonds" lists them */
	int wanted;      /* the number to find, or 0 to find an address */
	const bt_addr_le_t *addr;
	bt_addr_le_t found;
	int found_index; /* 0: not found */
};

static void lookup_bond(const struct bt_bond_info *info, void *user_data)
{
	struct bond_lookup *lookup = user_data;

	lookup->index++;
	if (lookup->wanted != 0 ? lookup->index == lookup->wanted
				: lookup->addr != NULL && bt_addr_le_eq(&info->addr, lookup->addr)) {
		bt_addr_le_copy(&lookup->found, &info->addr);
		lookup->found_index = lookup->index;
	}
}

/*
 * Makes sure a peer is selected and still paired. Returns its number in the
 * "bonds" list, or 0 after telling the user what is missing.
 */
static int selected_peer(const struct shell *sh)
{
	struct bond_lookup lookup = { .addr = have_selected ? &selected : NULL };

	bt_foreach_bond(BT_ID_DEFAULT, lookup_bond, &lookup);
	if (lookup.found_index != 0) {
		return lookup.found_index;
	}

	have_selected = false;
	if (lookup.index == 1) {
		/* The only one there is. */
		lookup = (struct bond_lookup){ .wanted = 1 };
		bt_foreach_bond(BT_ID_DEFAULT, lookup_bond, &lookup);
		bt_addr_le_copy(&selected, &lookup.found);
		have_selected = true;
		return 1;
	}

	if (lookup.index == 0) {
		shell_error(sh, "No device is paired yet");
	} else {
		shell_error(sh, "%d devices are paired. Choose one with: hid peer <number> "
				"(see \"bonds\")", lookup.index);
	}
	return 0;
}

/* The settings the command under way reads, and changes if it began with edit_begin(). */
static struct hid_profile *edit;
static struct peer_profile *edit_peer;

/* For a command that only reads the selected peer's settings. */
static bool view_begin(const struct shell *sh)
{
	if (selected_peer(sh) == 0) {
		return false;
	}
	edit_peer = peer_find(&selected);
	edit = edit_peer != NULL ? &edit_peer->profile : &defaults;
	return true;
}

static void find_unbonded(const struct bt_bond_info *info, void *user_data)
{
	ARRAY_FOR_EACH_PTR(peers, peer) {
		if (peer->used == 1 && bt_addr_le_eq(&peer->addr, &info->addr)) {
			peer->used = 2; /* marked: still bonded */
		}
	}
}

/*
 * For a command that changes the selected peer's settings: gives the peer
 * settings of its own, starting from the defaults, if it had none.
 */
static bool edit_begin(const struct shell *sh)
{
	if (!view_begin(sh)) {
		return false;
	}
	if (edit_peer != NULL) {
		return true;
	}

	ARRAY_FOR_EACH_PTR(peers, peer) {
		if (!peer->used) {
			edit_peer = peer;
			break;
		}
	}
	if (edit_peer == NULL) {
		/* Full of peers that are no longer paired: take one of theirs. */
		bt_foreach_bond(BT_ID_DEFAULT, find_unbonded, NULL);
		ARRAY_FOR_EACH_PTR(peers, peer) {
			if (peer->used == 1 && edit_peer == NULL) {
				edit_peer = peer;
			}
			peer->used = peer->used != 0;
		}
	}
	if (edit_peer == NULL) {
		shell_error(sh, "No room for this device's settings");
		return false;
	}

	bt_addr_le_copy(&edit_peer->addr, &selected);
	edit_peer->profile = defaults;
	edit_peer->used = 1;
	edit = &edit_peer->profile;
	return true;
}

static int peer_store(const struct peer_profile *peer)
{
	char key[sizeof("hid/peer/") + 3];

	snprintf(key, sizeof(key), "hid/peer/%u", (unsigned int)(peer - peers) & 0xff);

	return peer->used ? settings_save_one(key, peer, sizeof(*peer)) : settings_delete(key);
}

/* Saves what a command that began with edit_begin() changed. */
static int edit_save(const struct shell *sh)
{
	int err = peer_store(edit_peer);

	if (err) {
		shell_error(sh, "Applied, but not saved (err %d)", err);
	}
	return err;
}

/* A removed bond takes its settings with it. */
static void peer_bond_deleted(uint8_t id, const bt_addr_le_t *addr)
{
	struct peer_profile *peer = peer_find(addr);

	if (peer != NULL) {
		peer->used = 0;
		(void)peer_store(peer);
	}
	if (have_selected && bt_addr_le_eq(&selected, addr)) {
		have_selected = false;
	}
}

static struct bt_conn_auth_info_cb peer_callbacks = {
	.bond_deleted = peer_bond_deleted,
};

static int peers_init(void)
{
	return bt_conn_auth_info_cb_register(&peer_callbacks);
}

static int no_host(const struct shell *sh)
{
	shell_error(sh, "The device is not connected, or not listening as a HID host");
	return -ENOTCONN;
}

static bool check_enabled(const struct shell *sh, int index)
{
	if (!report_enabled(edit, index)) {
		shell_error(sh, "The %s report is off. Turn it on with: hid enable %s",
			    report_names[index], report_names[index]);
		return false;
	}
	return true;
}

static void list_reports(char *out, size_t size, uint32_t bits, int first_bit)
{
	out[0] = '\0';
	for (int i = 0; i < INPUT_COUNT; i++) {
		if (bits & BIT(first_bit + i)) {
			strncat(out, " ", size - strlen(out) - 1);
			strncat(out, report_names[i], size - strlen(out) - 1);
		}
	}
	if (out[0] == '\0') {
		strncat(out, " none", size - 1);
	}
}

static void print_link(struct bt_conn *conn, void *user_data)
{
	const struct shell *sh = user_data;
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0 || info.state != BT_CONN_STATE_CONNECTED) {
		return;
	}

	shell_print(sh, "link: interval %u.%02u ms, %s, MTU %u",
		    info.le.interval_us / 1000, (info.le.interval_us % 1000) / 10,
		    info.le.phy->tx_phy == BT_GAP_LE_PHY_2M ? "2M" : "1M",
		    bt_gatt_get_mtu(conn));
}

static void axis_name(int8_t axis, char name[3])
{
	name[0] = axis < 0 ? '-' : '+';
	name[1] = "xyz"[abs(axis) - 1];
	name[2] = '\0';
}

static void print_air(const struct shell *sh)
{
	char x[3];
	char y[3];
	char forward[3];
	char axes[8] = "auto";

	axis_name(edit->air.axis_x, x);
	axis_name(edit->air.axis_y, y);
	axis_name(edit->air.forward, forward);
	if (edit->air.manual) {
		snprintf(axes, sizeof(axes), "%s %s", x, y);
	}
	shell_print(sh, "air mouse %s, speed %u %u, accel %u deg/s, dead zone %u deg/s, "
			"axes %s, forward %s",
		    edit->air.on ? "on" : "off", edit->air.speed, edit->air.speed_y, edit->air.accel, edit->air.deadzone, axes,
		    forward);
}

/* Which peer the commands are about, and whether its settings are its own. */
static void print_peer(const struct shell *sh)
{
	char addr[BT_ADDR_LE_STR_LEN];
	const char *name = bonds_name(&selected);

	bt_addr_le_to_str(&selected, addr, sizeof(addr));
	shell_print(sh, "peer %d  %s%s%s%s, %s settings", selected_peer(sh), addr,
		    name ? "  \"" : "", name ? name : "", name ? "\"" : "",
		    edit_peer != NULL ? "own" : "default");
}

static int cmd_peer(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2) {
		struct bond_lookup lookup = { 0 };
		int err = 0;

		lookup.wanted = shell_strtol(argv[1], 10, &err);
		if (err || lookup.wanted < 1) {
			shell_error(sh, "Give a bond number from the \"bonds\" list");
			return -EINVAL;
		}
		bt_foreach_bond(BT_ID_DEFAULT, lookup_bond, &lookup);
		if (lookup.found_index == 0) {
			shell_error(sh, "No bond %d", lookup.wanted);
			return -ENOENT;
		}
		bt_addr_le_copy(&selected, &lookup.found);
		have_selected = true;
	}

	if (!view_begin(sh)) {
		return -ENOENT;
	}
	print_peer(sh);
	return 0;
}

static int cmd_hid(const struct shell *sh, size_t argc, char **argv)
{
	char list[48];

	if (!view_begin(sh)) {
		return -ENOENT;
	}
	print_peer(sh);
	list_reports(list, sizeof(list), edit->enabled, 0);
	shell_print(sh, "enabled:%s", list);
	/* Subscriptions are tracked by report ID, which starts at 1. */
	list_reports(list, sizeof(list), atomic_get(&subscribed), REPORT_ID_MOUSE);
	shell_print(sh, "hosts %d, listening to:%s", (int)atomic_get(&host_count), list);
	shell_print(sh, "sensor rate %d Hz", edit->rate);
	shell_print(sh, "keyboard LEDs 0x%02x", keyboard_leds);
	print_air(sh);
	bt_conn_foreach(BT_CONN_TYPE_LE, print_link, (void *)sh);

	return 0;
}

static int set_enabled(const struct shell *sh, const char *name, bool on)
{
	uint32_t mask = strcmp(name, "all") == 0 ? ALL_REPORTS : 0;

	for (int i = 0; i < INPUT_COUNT; i++) {
		if (strcmp(name, report_names[i]) == 0) {
			mask = BIT(i);
		}
	}
	if (mask == 0) {
		shell_error(sh, "Report must be mouse, keyboard, accel, gyro, orientation or all");
		return -EINVAL;
	}
	if (!edit_begin(sh)) {
		return -ENOENT;
	}

	if (on) {
		edit->enabled |= mask;
	} else {
		edit->enabled &= ~mask;
	}
	return edit_save(sh);
}

static int cmd_enable(const struct shell *sh, size_t argc, char **argv)
{
	return set_enabled(sh, argv[1], true);
}

static int cmd_disable(const struct shell *sh, size_t argc, char **argv)
{
	return set_enabled(sh, argv[1], false);
}

static int cmd_mouse_move(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t report[MOUSE_REPORT_LEN] = { 0 };
	int err = 0;
	long dx = shell_strtol(argv[1], 10, &err);
	long dy = shell_strtol(argv[2], 10, &err);

	if (!view_begin(sh)) {
		return -ENOENT;
	}
	if (!check_enabled(sh, INPUT_MOUSE)) {
		return -EACCES;
	}
	if (err || labs(dx) > 32767 || labs(dy) > 32767) {
		shell_error(sh, "Give x and y as numbers from -32767 to 32767");
		return -EINVAL;
	}

	sys_put_le16((int16_t)dx, &report[1]);
	sys_put_le16((int16_t)dy, &report[3]);

	return send_report(INPUT_MOUSE, report, sizeof(report)) > 0 ? 0 : no_host(sh);
}

static int cmd_mouse_click(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const buttons[] = { "left", "right", "middle" };
	uint8_t report[MOUSE_REPORT_LEN] = { BIT(0) };
	bool sent;

	if (!view_begin(sh)) {
		return -ENOENT;
	}
	if (!check_enabled(sh, INPUT_MOUSE)) {
		return -EACCES;
	}
	if (argc > 1) {
		report[0] = 0;
		ARRAY_FOR_EACH(buttons, i) {
			if (strcmp(argv[1], buttons[i]) == 0) {
				report[0] = BIT(i);
			}
		}
		if (report[0] == 0) {
			shell_error(sh, "Button must be left, right or middle");
			return -EINVAL;
		}
	}

	sent = send_report(INPUT_MOUSE, report, sizeof(report)) > 0;
	k_msleep(KEY_TAP_MS);
	report[0] = 0;
	send_report(INPUT_MOUSE, report, sizeof(report));

	return sent ? 0 : no_host(sh);
}

static int cmd_mouse_scroll(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t report[MOUSE_REPORT_LEN] = { 0 };
	int err = 0;
	long steps = shell_strtol(argv[1], 10, &err);

	if (!view_begin(sh)) {
		return -ENOENT;
	}
	if (!check_enabled(sh, INPUT_MOUSE)) {
		return -EACCES;
	}
	if (err || labs(steps) > 127) {
		shell_error(sh, "Give the number of steps, -127 to 127");
		return -EINVAL;
	}

	report[5] = (uint8_t)(int8_t)steps;

	return send_report(INPUT_MOUSE, report, sizeof(report)) > 0 ? 0 : no_host(sh);
}

/* "enter", "a", "ctrl+c", "shift+f5", "cmd+space", or a modifier alone. */
static int cmd_key(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t modifiers = 0;
	uint8_t usage = 0;
	char *part = argv[1];

	if (!view_begin(sh)) {
		return -ENOENT;
	}
	if (!check_enabled(sh, INPUT_KEYBOARD)) {
		return -EACCES;
	}

	while (part != NULL && *part != '\0') {
		char *plus = strlen(part) > 1 ? strchr(part + 1, '+') : NULL;
		uint8_t shift = 0;
		bool known = false;

		if (plus != NULL) {
			*plus = '\0';
		}

		ARRAY_FOR_EACH(named_modifiers, i) {
			if (strcmp(part, named_modifiers[i].name) == 0) {
				modifiers |= named_modifiers[i].modifier;
				known = true;
			}
		}
		ARRAY_FOR_EACH(named_keys, i) {
			if (!known && usage == 0 && strcmp(part, named_keys[i].name) == 0) {
				usage = named_keys[i].usage;
				known = true;
			}
		}
		if (!known && usage == 0 && strlen(part) == 1 &&
		    ascii_to_key(part[0], &usage, &shift)) {
			modifiers |= shift;
			known = true;
		}
		if (!known) {
			shell_error(sh, "Unknown key \"%s\". Use a character, a name such as "
				    "enter, esc, tab, space, up, f1, or ctrl/shift/alt/gui "
				    "joined with +", part);
			return -EINVAL;
		}

		part = plus != NULL ? plus + 1 : NULL;
	}

	return tap_key(modifiers, usage) ? 0 : no_host(sh);
}

static int cmd_type(const struct shell *sh, size_t argc, char **argv)
{
	if (!view_begin(sh)) {
		return -ENOENT;
	}
	if (!check_enabled(sh, INPUT_KEYBOARD)) {
		return -EACCES;
	}

	for (size_t arg = 1; arg < argc; arg++) {
		const char *text = argv[arg];

		if (arg > 1 && !tap_key(0, 0x2c)) {
			return no_host(sh);
		}

		for (; *text != '\0'; text++) {
			uint8_t usage;
			uint8_t modifiers;

			if (!ascii_to_key(*text, &usage, &modifiers)) {
				shell_error(sh, "Cannot type character 0x%02x", (uint8_t)*text);
				return -EINVAL;
			}
			if (!tap_key(modifiers, usage)) {
				return no_host(sh);
			}
		}
	}

	return 0;
}

static int cmd_rate(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	unsigned long value = shell_strtoul(argv[1], 10, &err);
	uint8_t rate = value;

	if (err || value < SENSOR_RATE_MIN || value > SENSOR_RATE_MAX) {
		shell_error(sh, "Rate must be %d to %d (Hz)", SENSOR_RATE_MIN, SENSOR_RATE_MAX);
		return -EINVAL;
	}

	if (!edit_begin(sh)) {
		return -ENOENT;
	}
	edit->rate = rate;
	/* What a host asked for no longer stands in the way. */
	atomic_set(&host_rate_hz, 0);
	return edit_save(sh);
}


static int cmd_air(const struct shell *sh, size_t argc, char **argv)
{
	if (!view_begin(sh)) {
		return -ENOENT;
	}
	print_air(sh);
	return 0;
}

static int cmd_air_on(const struct shell *sh, size_t argc, char **argv)
{
	if (!edit_begin(sh)) {
		return -ENOENT;
	}
	if (!check_enabled(sh, INPUT_MOUSE)) {
		return -EACCES;
	}
	edit->air.on = 1;
	return edit_save(sh);
}

static int cmd_air_off(const struct shell *sh, size_t argc, char **argv)
{
	if (!edit_begin(sh)) {
		return -ENOENT;
	}
	edit->air.on = 0;
	return edit_save(sh);
}

static int cmd_air_speed(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	unsigned long x = shell_strtoul(argv[1], 10, &err);
	unsigned long y = argc == 3 ? shell_strtoul(argv[2], 10, &err) : x;

	if (err || x < AIR_SPEED_MIN || x > AIR_SPEED_MAX ||
	    y < AIR_SPEED_MIN || y > AIR_SPEED_MAX) {
		shell_error(sh, "Speed must be %d to %d (pointer counts per degree turned), "
				"one value for both directions or one each for X and Y",
			    AIR_SPEED_MIN, AIR_SPEED_MAX);
		return -EINVAL;
	}
	if (!edit_begin(sh)) {
		return -ENOENT;
	}
	edit->air.speed = x;
	edit->air.speed_y = y;
	return edit_save(sh);
}

static int cmd_air_accel(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	unsigned long value = shell_strtoul(argv[1], 10, &err);

	if (err || value > AIR_ACCEL_MAX) {
		shell_error(sh, "Give the turn rate, 1 to %d deg/s, at which the pointer moves "
				"at the set speed, or 0 for no acceleration", AIR_ACCEL_MAX);
		return -EINVAL;
	}
	if (!edit_begin(sh)) {
		return -ENOENT;
	}
	edit->air.accel = value;
	return edit_save(sh);
}

static int cmd_air_deadzone(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	unsigned long value = shell_strtoul(argv[1], 10, &err);

	if (err || value > AIR_DEADZONE_MAX) {
		shell_error(sh, "Dead zone must be 0 to %d (deg/s)", AIR_DEADZONE_MAX);
		return -EINVAL;
	}
	if (!edit_begin(sh)) {
		return -ENOENT;
	}
	edit->air.deadzone = value;
	return edit_save(sh);
}

/* "x", "+x", "-x" and so on: 0 if the text is not an axis. */
static int8_t parse_axis(const char *text)
{
	int8_t sign = 1;
	const char *letter;

	if (*text == '+' || *text == '-') {
		sign = *text == '-' ? -1 : 1;
		text++;
	}
	letter = *text != '\0' && text[1] == '\0' ? strchr("xyz", *text) : NULL;

	return letter != NULL ? sign * (int8_t)(letter - "xyz" + 1) : 0;
}

static int cmd_air_axes(const struct shell *sh, size_t argc, char **argv)
{
	int8_t x = parse_axis(argv[1]);
	int8_t y = argc == 3 ? parse_axis(argv[2]) : 0;

	if (!edit_begin(sh)) {
		return -ENOENT;
	}
	if (argc == 2 && strcmp(argv[1], "auto") == 0) {
		edit->air.manual = 0;
		return edit_save(sh);
	}
	if (x == 0 || y == 0) {
		shell_error(sh, "Give auto, or the gyroscope axis for pointer X and for "
				"pointer Y, each one of: +x -x +y -y +z -z");
		return -EINVAL;
	}
	edit->air.axis_x = x;
	edit->air.axis_y = y;
	edit->air.manual = 1;
	return edit_save(sh);
}

static int cmd_air_forward(const struct shell *sh, size_t argc, char **argv)
{
	int8_t forward = parse_axis(argv[1]);

	if (forward == 0) {
		shell_error(sh, "Give the board axis that points at the screen, one of: "
				"+x -x +y -y +z -z");
		return -EINVAL;
	}
	if (!edit_begin(sh)) {
		return -ENOENT;
	}
	edit->air.forward = forward;
	return edit_save(sh);
}

SHELL_STATIC_SUBCMD_SET_CREATE(air_cmds,
	SHELL_CMD(on, NULL, "Move the pointer by turning the board", cmd_air_on),
	SHELL_CMD(off, NULL, "Stop moving the pointer", cmd_air_off),
	SHELL_CMD_ARG(speed, NULL, "<x> [y]  Pointer counts per degree turned, 1-200: one "
		      "value for both directions, or X then Y", cmd_air_speed, 2, 1),
	SHELL_CMD_ARG(accel, NULL, "<0-250>  Quadratic acceleration: the turn rate, in deg/s, "
		      "at which the pointer moves at the set speed. 0 turns it off",
		      cmd_air_accel, 2, 0),
	SHELL_CMD_ARG(deadzone, NULL, "<0-20>  Ignore turning slower than this, in deg/s",
		      cmd_air_deadzone, 2, 0),
	SHELL_CMD_ARG(axes, NULL, "auto | <x> <y>  Work the axes out from gravity, or name the "
		      "gyroscope axis for pointer X and Y (+x -x +y -y +z -z)", cmd_air_axes, 2, 1),
	SHELL_CMD_ARG(forward, NULL, "<axis>  Board axis that points at the screen, for "
		      "automatic axes (+x -x +y -y +z -z)", cmd_air_forward, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(mouse_cmds,
	SHELL_CMD_ARG(move, NULL, "<x> <y>  Move the pointer", cmd_mouse_move, 3, 0),
	SHELL_CMD_ARG(click, NULL, "[left|right|middle]  Click a button", cmd_mouse_click, 1, 1),
	SHELL_CMD_ARG(scroll, NULL, "<steps>  Turn the wheel", cmd_mouse_scroll, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(hid_cmds,
	SHELL_CMD_ARG(peer, NULL, "[number]  Show or choose the paired device the other "
		      "commands are about, by its number in \"bonds\"", cmd_peer, 1, 1),
	SHELL_CMD_ARG(enable, NULL, "<mouse|keyboard|accel|gyro|orientation|all>  Turn a report on",
		      cmd_enable, 2, 0),
	SHELL_CMD_ARG(disable, NULL, "<mouse|keyboard|accel|gyro|orientation|all>  Turn a report off",
		      cmd_disable, 2, 0),
	SHELL_CMD(mouse, &mouse_cmds, "Send mouse movement, clicks and scrolling", NULL),
	SHELL_CMD(air, &air_cmds, "Air mouse: show or change its settings", cmd_air),
	SHELL_CMD_ARG(key, NULL, "<key>  Tap a key, e.g. enter, a, ctrl+c, shift+f5", cmd_key, 2, 0),
	SHELL_CMD_ARG(type, NULL, "<text>  Type text (US keyboard layout)", cmd_type, 2,
		      SHELL_OPT_ARG_CHECK_SKIP),
	SHELL_CMD_ARG(rate, NULL, "<Hz>  Rate of the accelerometer, gyroscope and orientation "
		      "reports", cmd_rate, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(hid, &hid_cmds, "Bluetooth HID: mouse, keyboard and sensor reports", cmd_hid);
