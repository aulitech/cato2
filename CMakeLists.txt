# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The Sense Plus shares the xiao_ble_sense board definition (same nRF52840,
# LEDs, IMU and mic; the Plus only adds extra castellated pads).
if(NOT DEFINED BOARD)
  set(BOARD xiao_ble_sense)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xiao_nrf52840_sense_plus)

target_sources(app PRIVATE src/main.c)
