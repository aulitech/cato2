/*
 * SPDX-License-Identifier: MIT
 */

#ifndef APP_H_
#define APP_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/uuid.h>

struct bt_conn;

/* Nordic UART Service, used for the Bluetooth shell and advertised. */
#define APP_UUID_NUS_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

/*
 * ble_shell.c: start the Bluetooth shell (once, after Bluetooth is enabled).
 * Every connected device that subscribes shares the one shell.
 */
int ble_shell_init(void);

/* ble_shell.c: change the Bluetooth shell's prompt. */
int ble_shell_set_prompt(const char *prompt);

/*
 * hid.c: register the HID service (before Bluetooth is enabled) and tell it
 * about each connection.
 */
int hid_init(void);
int hid_connected(struct bt_conn *conn);
void hid_disconnected(struct bt_conn *conn);

/* blink.c: turn the LED off and stop blinking, ahead of power-off. */
void blink_stop(void);

/*
 * imu.c: reset the IMU and apply the saved (or default) configuration. Call
 * after settings have been loaded.
 */
int imu_init(void);

/* One reading with the current ranges: acceleration in g, rotation in deg/s. */
int imu_sample(float accel_g[3], float gyro_dps[3]);

/* Put the accelerometer and gyroscope in power-down. */
int imu_power_down(void);

/*
 * Leave only the low-power accelerometer running and raise INT1 when the
 * acceleration changes by more than threshold_mg. The threshold actually set
 * (rounded to what the chip supports) is written back.
 */
int imu_arm_wake(uint32_t *threshold_mg);

/* bonds.c: register the pairing policy. Call before Bluetooth is enabled. */
int bonds_init(void);

#include <zephyr/bluetooth/addr.h>

/* bonds.c: the name given to a bonded device, or NULL if it has none. */
const char *bonds_name(const bt_addr_le_t *addr);

/* crash.c: log the crash that caused the previous restart, if any. */
void crash_report(void);

#endif /* APP_H_ */
