# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

# The Sense Plus shares the XIAO BLE Sense board definition (same nRF52840,
# LEDs, IMU and mic; the Plus only adds extra pads on the underside).
if(NOT DEFINED BOARD)
  set(BOARD xiao_ble/nrf52840/sense)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xiao_nrf52840_sense_plus)

target_sources(app PRIVATE
  src/main.c
  src/ble_shell.c
  src/blink.c
  src/bonds.c
  src/crash.c
  src/files.c
  src/hid.c
  src/imu.c
  src/sleep.c
)
