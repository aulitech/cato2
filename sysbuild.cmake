# SPDX-License-Identifier: MIT
#
# Produce build/first_flash.uf2: MCUboot plus the signed application in one
# file, for drag-and-drop onto the Adafruit UF2 bootloader drive. The per-image
# zephyr.uf2 files are not enough on their own (the application one is
# unsigned, so MCUboot would refuse to boot it).

set(first_flash_hex ${CMAKE_BINARY_DIR}/first_flash.hex)
set(first_flash_uf2 ${CMAKE_BINARY_DIR}/first_flash.uf2)

add_custom_target(first_flash_uf2 ALL
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/mergehex.py
          -o ${first_flash_hex}
          ${CMAKE_BINARY_DIR}/mcuboot/zephyr/zephyr.hex
          ${CMAKE_BINARY_DIR}/${DEFAULT_IMAGE}/zephyr/zephyr.signed.hex
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/uf2conv.py
          -c -f 0xada52840 -o ${first_flash_uf2} ${first_flash_hex}
  DEPENDS mcuboot ${DEFAULT_IMAGE}
  COMMENT "Generating first_flash.uf2"
  VERBATIM
)
