# SPDX-License-Identifier: MIT
#
# Produce build/usb-upgrade.uf2: MCUboot plus the signed application in one
# file, for drag-and-drop onto the Adafruit UF2 bootloader drive. It starts
# right after the MBR, where a board as it comes has the SoftDevice: the
# bootloader writes over that and from then on starts MCUboot instead. The per-image
# zephyr.uf2 files are not enough on their own (the application one is
# unsigned, so MCUboot would refuse to boot it).

set(usb_upgrade_hex ${CMAKE_BINARY_DIR}/usb-upgrade.hex)
set(usb_upgrade_uf2 ${CMAKE_BINARY_DIR}/usb-upgrade.uf2)
set(usb_upgrade_blank ${CMAKE_BINARY_DIR}/usb-upgrade-blank.hex)

# A board that had another flash layout before (the one with the SoftDevice
# kept, or other firmware altogether) has old data where MCUboot now keeps its
# notes on the two slots: the last pages of each, and the start of slot 1. The
# file erases those, or MCUboot and the firmware would read the leftovers as
# an image waiting or a swap under way. As in dts/mcuboot_partitions.dtsi:
# slot 0 ends and slot 1 begins at 0x79000, and slot 1 ends at 0xec000.
#
# The end of slot 1 cannot be erased this way: the UF2 bootloader keeps
# everything from 0xea000 up for the application's own data and refuses a
# file that writes there. The firmware sees to slot 1's last page itself, the
# first time it starts (storage_follow_layout() in src/main.c): on a board
# from before the settings moved up a page, that page is the first of them.
set(usb_upgrade_blank_ranges 0x77000:0x7a000)

add_custom_target(usb_upgrade_uf2 ALL
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/blank_hex.py
          ${usb_upgrade_blank} ${usb_upgrade_blank_ranges}
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/mergehex.py
          -o ${usb_upgrade_hex}
          ${CMAKE_BINARY_DIR}/mcuboot/zephyr/zephyr.hex
          ${CMAKE_BINARY_DIR}/${DEFAULT_IMAGE}/zephyr/zephyr.signed.hex
          ${usb_upgrade_blank}
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/uf2conv.py
          -c -f 0xada52840 -o ${usb_upgrade_uf2} ${usb_upgrade_hex}
  DEPENDS mcuboot ${DEFAULT_IMAGE}
  COMMENT "Generating usb-upgrade.uf2"
  VERBATIM
)

# Produce build/cato.zip: the Bluetooth (OTA) upgrade package, which the SDK
# builds as dfu_application.zip, under the name it is published with.
add_custom_target(cato_zip ALL
  COMMAND ${CMAKE_COMMAND} -E copy
          ${CMAKE_BINARY_DIR}/dfu_application.zip ${CMAKE_BINARY_DIR}/cato.zip
  DEPENDS dfu_application_zip
  COMMENT "Generating cato.zip"
  VERBATIM
)
