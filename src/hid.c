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
 */

#include <math.h>
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

/* Reports switched on with "hid enable" (bit per INPUT_ index). All off by default. */
static atomic_t enabled;

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
static atomic_t sensor_rate_hz = ATOMIC_INIT(SENSOR_RATE_DEFAULT);

static bool report_enabled(int index)
{
	return atomic_test_bit(&enabled, index);
}

/* A sensor report goes out if it is enabled and the host wants its events. */
static bool sensor_wanted(int index)
{
	uint8_t reporting = host_request[index].reporting_state;

	return report_enabled(index) && host_request[index].power_state != POWER_D4_OFF &&
	       reporting != REPORTING_NO_EVENTS && reporting != REPORTING_NO_EVENTS_WAKE;
}
static uint8_t keyboard_leds;

/* ------------------------------------------------------------- sending --- */

struct report {
	uint8_t index;
	const uint8_t *data;
	uint8_t len;
	int sent;
};

static void send_to(struct bt_conn *conn, void *user_data)
{
	struct report *report = user_data;

	/* Fails for a host that has not subscribed to this report: ignored. */
	if (bt_hids_inp_rep_send(report_route[report->index].service, conn,
				 report_route[report->index].index, report->data, report->len,
				 NULL) == 0) {
		report->sent++;
	}
}

/* Sends an input report to every host. Returns how many accepted it. */
static int send_report(uint8_t index, const uint8_t *data, uint8_t len)
{
	struct report report = { .index = index, .data = data, .len = len };

	bt_conn_foreach(BT_CONN_TYPE_LE, send_to, &report);
	return report.sent;
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
static void sensor_feature(int index, struct bt_hids_rep *rep, bool write)
{
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
			atomic_set(&sensor_rate_hz,
				   CLAMP(1000 / interval_ms, SENSOR_RATE_MIN, SENSOR_RATE_MAX));
		}
		return;
	}

	memset(data, 0, SENSOR_FEATURE_LEN);
	data[FEATURE_CONNECTION_TYPE] = CONNECTION_PC_EXTERNAL;
	data[FEATURE_REPORTING_STATE] = host_request[index].reporting_state;
	data[FEATURE_POWER_STATE] = host_request[index].power_state;
	data[FEATURE_SENSOR_STATE] = report_enabled(index) ? SENSOR_STATE_READY
							  : SENSOR_STATE_NOT_AVAILABLE;
	sys_put_le32(1000 / atomic_get(&sensor_rate_hz), &data[FEATURE_REPORT_INTERVAL]);
	sys_put_le16(0, &data[FEATURE_SENSITIVITY]);
}

static void on_accel_feature(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	sensor_feature(INPUT_ACCEL, rep, write);
}

static void on_gyro_feature(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	sensor_feature(INPUT_GYRO, rep, write);
}

static void on_orientation_feature(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	sensor_feature(INPUT_ORIENTATION, rep, write);
}

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

static void send_sensor_reports(const float accel_g[3], const float gyro_dps[3])
{
	uint8_t report[ORIENTATION_REPORT_LEN] = { SENSOR_STATE_READY, SENSOR_EVENT_DATA_UPDATED };
	const float quaternion[4] = { q.x, q.y, q.z, q.w };

	if (sensor_wanted(INPUT_ACCEL)) {
		for (int i = 0; i < 3; i++) {
			sys_put_le16(to_int16(accel_g[i] * 1000.0f), &report[2 + 2 * i]);
		}
		send_report(INPUT_ACCEL, report, SENSOR3_REPORT_LEN);
	}

	if (sensor_wanted(INPUT_GYRO)) {
		for (int i = 0; i < 3; i++) {
			sys_put_le16(to_int16(gyro_dps[i] * 10.0f), &report[2 + 2 * i]);
		}
		send_report(INPUT_GYRO, report, SENSOR3_REPORT_LEN);
	}

	if (sensor_wanted(INPUT_ORIENTATION)) {
		for (int i = 0; i < 4; i++) {
			sys_put_le16(to_int16(quaternion[i] * 10000.0f), &report[2 + 2 * i]);
		}
		send_report(INPUT_ORIENTATION, report, ORIENTATION_REPORT_LEN);
	}
}

static void sensor_thread(void *p1, void *p2, void *p3)
{
	const float deg_to_rad = 0.017453293f;
	int64_t next = k_uptime_get();
	int64_t last_sample = 0;
	bool streaming = false;
	int since_report = 0;

	while (1) {
		int rate = atomic_get(&sensor_rate_hz);
		float accel_g[3];
		float gyro_dps[3];

		if (atomic_get(&host_count) == 0 ||
		    !(sensor_wanted(INPUT_ACCEL) || sensor_wanted(INPUT_GYRO) ||
		      sensor_wanted(INPUT_ORIENTATION))) {
			streaming = false;
			k_msleep(200);
			continue;
		}

		if (!streaming) {
			streaming = true;
			since_report = 0;
			next = k_uptime_get();
			last_sample = next - 1000 / SAMPLE_HZ;
			fusion_reset();
			bias_reset_window();
		}

		if (imu_sample(accel_g, gyro_dps) == 0) {
			int64_t now = k_uptime_get();

			bias_update(accel_g, gyro_dps);

			/* Use the time that really passed: a slow link can delay the loop. */
			fusion_update((gyro_dps[0] - gyro_bias[0]) * deg_to_rad,
				      (gyro_dps[1] - gyro_bias[1]) * deg_to_rad,
				      (gyro_dps[2] - gyro_bias[2]) * deg_to_rad,
				      accel_g[0], accel_g[1], accel_g[2],
				      (now - last_sample) / 1000.0f);
			last_sample = now;

			since_report += rate;
			if (since_report >= SAMPLE_HZ) {
				since_report -= SAMPLE_HZ;
				send_sensor_reports(accel_g, gyro_dps);
			}
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
	uint8_t value;
	ssize_t rc;

	if (len != sizeof(value)) {
		return -EINVAL;
	}
	rc = read_cb(cb_arg, &value, sizeof(value));
	if (rc < 0) {
		return rc;
	}

	if (settings_name_steq(name, "enabled", NULL)) {
		atomic_set(&enabled, value & ALL_REPORTS);
		return 0;
	}
	if (settings_name_steq(name, "rate", NULL)) {
		if (value >= SENSOR_RATE_MIN && value <= SENSOR_RATE_MAX) {
			atomic_set(&sensor_rate_hz, value);
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

static int no_host(const struct shell *sh)
{
	shell_error(sh, "No HID host is connected and listening");
	return -ENOTCONN;
}

static bool check_enabled(const struct shell *sh, int index)
{
	if (!report_enabled(index)) {
		shell_error(sh, "The %s report is off. Turn it on with: hid enable %s",
			    report_names[index], report_names[index]);
		return false;
	}
	return true;
}

static void list_reports(char *out, size_t size, atomic_t *bits, int first_bit)
{
	out[0] = '\0';
	for (int i = 0; i < INPUT_COUNT; i++) {
		if (atomic_test_bit(bits, first_bit + i)) {
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

static int cmd_hid(const struct shell *sh, size_t argc, char **argv)
{
	char list[48];

	list_reports(list, sizeof(list), &enabled, 0);
	shell_print(sh, "enabled:%s", list);
	/* Subscriptions are tracked by report ID, which starts at 1. */
	list_reports(list, sizeof(list), &subscribed, REPORT_ID_MOUSE);
	shell_print(sh, "hosts %d, listening to:%s", (int)atomic_get(&host_count), list);
	shell_print(sh, "sensor rate %d Hz", (int)atomic_get(&sensor_rate_hz));
	shell_print(sh, "keyboard LEDs 0x%02x", keyboard_leds);
	bt_conn_foreach(BT_CONN_TYPE_LE, print_link, (void *)sh);

	return 0;
}

static int set_enabled(const struct shell *sh, const char *name, bool on)
{
	uint32_t mask = strcmp(name, "all") == 0 ? ALL_REPORTS : 0;
	uint8_t value;
	int err;

	for (int i = 0; i < INPUT_COUNT; i++) {
		if (strcmp(name, report_names[i]) == 0) {
			mask = BIT(i);
		}
	}
	if (mask == 0) {
		shell_error(sh, "Report must be mouse, keyboard, accel, gyro, orientation or all");
		return -EINVAL;
	}

	if (on) {
		atomic_or(&enabled, mask);
	} else {
		atomic_and(&enabled, ~mask);
	}

	value = atomic_get(&enabled);
	err = settings_save_one("hid/enabled", &value, sizeof(value));
	if (err) {
		shell_error(sh, "Applied, but not saved (err %d)", err);
	}
	return err;
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

	atomic_set(&sensor_rate_hz, rate);

	err = settings_save_one("hid/rate", &rate, sizeof(rate));
	if (err) {
		shell_error(sh, "Applied, but not saved (err %d)", err);
	}
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mouse_cmds,
	SHELL_CMD_ARG(move, NULL, "<x> <y>  Move the pointer", cmd_mouse_move, 3, 0),
	SHELL_CMD_ARG(click, NULL, "[left|right|middle]  Click a button", cmd_mouse_click, 1, 1),
	SHELL_CMD_ARG(scroll, NULL, "<steps>  Turn the wheel", cmd_mouse_scroll, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(hid_cmds,
	SHELL_CMD_ARG(enable, NULL, "<mouse|keyboard|accel|gyro|orientation|all>  Turn a report on",
		      cmd_enable, 2, 0),
	SHELL_CMD_ARG(disable, NULL, "<mouse|keyboard|accel|gyro|orientation|all>  Turn a report off",
		      cmd_disable, 2, 0),
	SHELL_CMD(mouse, &mouse_cmds, "Send mouse movement, clicks and scrolling", NULL),
	SHELL_CMD_ARG(key, NULL, "<key>  Tap a key, e.g. enter, a, ctrl+c, shift+f5", cmd_key, 2, 0),
	SHELL_CMD_ARG(type, NULL, "<text>  Type text (US keyboard layout)", cmd_type, 2,
		      SHELL_OPT_ARG_CHECK_SKIP),
	SHELL_CMD_ARG(rate, NULL, "<Hz>  Rate of the accelerometer, gyroscope and orientation "
		      "reports", cmd_rate, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(hid, &hid_cmds, "Bluetooth HID: mouse, keyboard and sensor reports", cmd_hid);
