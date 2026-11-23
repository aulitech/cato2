# xiao-nrf52840-sense-plus

Zephyr / nRF Connect SDK starter application for the
[Seeed Studio XIAO nRF52840 Sense Plus](https://wiki.seeedstudio.com/XIAO_BLE/).

It blinks the red LED and logs accelerometer readings from the on-board
LSM6DS3TR-C IMU over the USB serial console.

## Requirements

- nRF Connect SDK v2.5.3 (Zephyr 3.4) and its toolchain
- The board's stock Adafruit nRF52 UF2 bootloader (no debug probe needed)

## Board target

The app builds for `xiao_ble_sense`. NCS v2.5.3 has no dedicated Sense Plus
board; the Plus uses the same nRF52840, LEDs, IMU and microphone as the Sense
and adds extra pads on the underside. To use those extra pins, add them in an
`app.overlay`.

## Build

From an nRF Connect SDK terminal:

```sh
west build -b xiao_ble_sense
```

Or add the folder as an application in the nRF Connect for VS Code extension
and create a build configuration for `xiao_ble_sense`.

## Flash

1. Double-tap the reset button. The board mounts as a USB drive named
   `XIAO-SENSE`.
2. Copy `build/zephyr/zephyr.uf2` onto the drive, or run `west flash -r uf2`.

## Console

Open the USB CDC ACM serial port that appears after flashing (any baud rate):

```
[00:00:01.003,000] <inf> app: accel x=0.12 y=-0.34 z=9.79 m/s^2
```
