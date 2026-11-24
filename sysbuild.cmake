# SPDX-License-Identifier: MIT
#
# Produce build/usb-upgrade.uf2: MCUboot plus the signed application in one
# file, for drag-and-drop onto the Adafruit UF2 bootloader drive. The per-image
# zephyr.uf2 files are not enough on their own (the application one is
# unsigned, so MCUboot would refuse to boot it).

set(usb_upgrade_hex ${CMAKE_BINARY_DIR}/usb-upgrade.hex)
set(usb_upgrade_uf2 ${CMAKE_BINARY_DIR}/usb-upgrade.uf2)

add_custom_target(usb_upgrade_uf2 ALL
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/mergehex.py
          -o ${usb_upgrade_hex}
          ${CMAKE_BINARY_DIR}/mcuboot/zephyr/zephyr.hex
          ${CMAKE_BINARY_DIR}/${DEFAULT_IMAGE}/zephyr/zephyr.signed.hex
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
