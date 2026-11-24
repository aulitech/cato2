/*
 * SPDX-License-Identifier: MIT
 */

#ifndef APP_H_
#define APP_H_

#include <stdbool.h>
#include <stdint.h>

/* blink.c: turn the LED off and stop blinking, ahead of power-off. */
void blink_stop(void);

/*
 * imu.c: reset the IMU and apply the saved (or default) configuration. Call
 * after settings have been loaded.
 */
int imu_init(void);

/* Put the accelerometer and gyroscope in power-down. */
int imu_power_down(void);

/*
 * Leave only the low-power accelerometer running and raise INT1 when the
 * acceleration changes by more than threshold_mg. The threshold actually set
 * (rounded to what the chip supports) is written back.
 */
int imu_arm_wake(uint32_t *threshold_mg);

#endif /* APP_H_ */
