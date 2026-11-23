# xiao-nrf52840-sense-plus

Zephyr / nRF Connect SDK starter application for the
[Seeed Studio XIAO nRF52840 Sense Plus](https://wiki.seeedstudio.com/XIAO_BLE/).

- **BLE console** – the Zephyr shell and log output over the Nordic UART
  Service (NUS)
- **OTA upgrade** – MCUmgr/SMP over Bluetooth, with MCUboot swapping in the new
  image and reverting it if it fails to start
- **No debug probe needed** – MCUboot is chain-loaded by the board's stock
  Adafruit UF2 bootloader, so the first flash is drag-and-drop

The app blinks the red LED and adds an `accel` shell command that reads the
on-board LSM6DS3TR-C IMU.

> **Not yet verified on hardware.** Everything builds, but the firmware has not
> been run on a board.

## Requirements

- nRF Connect SDK v3.4.2 and its toolchain
- The board's stock Adafruit nRF52 UF2 bootloader (with SoftDevice S140 v7)

## Board target

The app builds for `xiao_ble/nrf52840/sense`. The SDK has no dedicated Sense
Plus board; the Plus uses the same nRF52840, LEDs, IMU and microphone as the
Sense and adds extra pads on the underside. To use those extra pins, add them
to `app.overlay`.

## Build

From an nRF Connect SDK terminal:

```sh
west build -b xiao_ble/nrf52840/sense --sysbuild
```

Or add the folder as an application in the nRF Connect for VS Code extension
and create a build configuration for `xiao_ble/nrf52840/sense` (sysbuild is
picked up automatically).

Outputs:

| File | Use |
| --- | --- |
| `build/first_flash.uf2` | MCUboot + signed app, for the first flash over USB |
| `build/dfu_application.zip` | OTA update package |

## First flash (USB)

1. Double-tap the reset button. The board mounts as a USB drive named
   `XIAO-SENSE`.
2. Copy `build/first_flash.uf2` onto the drive.

The same procedure recovers a board at any time, since the UF2 bootloader is
left untouched.

## Console (BLE)

Connect to **XIAO Sense Plus** with any Nordic UART Service terminal, e.g.
the UART feature in nRF Toolbox or the Bluefruit Connect app, and type `help`.

```
bt_nus:~$ accel
accel x=0.12 y=-0.34 z=9.79 m/s^2
```

Log output is also sent to the USB CDC ACM serial port.

## OTA upgrade (BLE)

1. Bump the version in `VERSION` and rebuild.
2. Open **nRF Connect Device Manager** (iOS/Android), connect to
   **XIAO Sense Plus**, and upload `build/dfu_application.zip`.

MCUboot swaps the new image in on the next reset. The app confirms the image
once Bluetooth is up; an image that never gets that far is reverted on the
following reset.

## Flash layout

| Address | Size | Contents |
| --- | --- | --- |
| `0x00000` | 156 KB | MBR + SoftDevice (left in place, unused by Zephyr) |
| `0x27000` | 48 KB | MCUboot |
| `0x33000` | 368 KB | Slot 0 – running image |
| `0x8F000` | 368 KB | Slot 1 – OTA download |
| `0xEB000` | 36 KB | Settings storage |
| `0xF4000` | 48 KB | Adafruit UF2 bootloader |

Defined in `dts/mcuboot_partitions.dtsi`, shared by the app and MCUboot.

## Before shipping a product

- **Signing key** – images are signed with MCUboot's public default key, so
  anyone can sign an image the bootloader will accept. Generate your own key and
  set `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` in `sysbuild.conf` (keep the key out
  of the repository).
- **Open access** – the console and the OTA service accept unauthenticated,
  unencrypted connections from any device in range. Require pairing before
  deploying.

## License

[MIT](LICENSE)
