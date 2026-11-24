# xiao-nrf52840-sense-plus

Zephyr / nRF Connect SDK starter application for the
[Seeed Studio XIAO nRF52840 Sense Plus](https://wiki.seeedstudio.com/XIAO_BLE/).

- **Console** – the Zephyr shell and log output over the Nordic UART Service
  (NUS) and over USB serial
- **OTA upgrade** – MCUmgr/SMP over Bluetooth, with MCUboot swapping in the new
  image and reverting it if it fails to start
- **No debug probe needed** – MCUboot is chain-loaded by the board's stock
  Adafruit UF2 bootloader, so the first flash is drag-and-drop

The app blinks the RGB LED and adds shell commands to set the blink rate and
pattern (`blink`), rename the board (`name`), read and fully configure the
on-board LSM6DS3TR-C IMU (`imu`), sleep until reset or motion (`sleep`), and
restart into the USB bootloader (`bootloader`).

> **Verified on hardware** (XIAO nRF52840 Sense, UF2 bootloader 0.6.1): UF2 first
> flash, MCUboot chain-load, the shell over BLE and USB, the `accel` and `blink`
> commands (including persistence), and OTA upgrades
> in both directions. MCUboot's automatic revert of a bad image has not been
> exercised.

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

A board already running this firmware can skip the button: the `bootloader`
shell command resets it into the same USB drive mode.

The same procedure recovers a board at any time, since the UF2 bootloader is
left untouched.

## Console

The Zephyr shell (with log output) is available over both Bluetooth and USB.
Type `help` for the command list.

```
bt_nus:~$ accel
accel x=0.12 y=-0.34 z=9.79 m/s^2
```

### Blink

The `blink` command shows or changes how the RGB LED blinks. Changes take
effect immediately and are kept across resets and OTA upgrades.

```
bt_nus:~$ blink
rate 1000 ms, pattern rgb
bt_nus:~$ blink rate 250
bt_nus:~$ blink pattern r-b-w
```

- `blink rate <ms>` – time per pattern step, 20 to 60000 ms
- `blink pattern <steps>` – up to 32 steps, one character each: `r` `g` `b`
  red, green, blue; `y` `c` `m` yellow, cyan, magenta; `w` white; `-` off

### Name

`name` shows the board's Bluetooth name and `name <new name>` changes it, up
to 26 characters. The name is kept across resets. Phones and computers cache
names, so a board they have already seen may show its old name for a while.

### IMU

`imu` shows the accelerometer and gyroscope configuration and `imu read` takes
one reading of acceleration, rotation rate and temperature (`accel` reads the
accelerometer alone).

```
bt_nus:~$ imu
accel  odr 104 Hz, range +-2 g, high-performance mode
gyro   odr 104 Hz, range +-250 dps, high-performance mode
saved  no (defaults at boot)
bt_nus:~$ imu accel range 8
bt_nus:~$ imu gyro odr 0
bt_nus:~$ imu save
```

| Command | Values |
| --- | --- |
| `imu accel odr <Hz>`, `imu gyro odr <Hz>` | `0` (off) `12.5` `26` `52` `104` `208` `416` `833` `1660` `3330` `6660` |
| `imu accel range <g>` | `2` `4` `8` `16` |
| `imu gyro range <dps>` | `125` `250` `500` `1000` `2000` |
| `imu accel mode <high\|low>`, `imu gyro mode <high\|low>` | high-performance or low-power |
| `imu reg read <register> [count]` | any register, `0x00` to `0x7f` |
| `imu reg write <register> <value>` | any register; see the LSM6DS3TR-C datasheet |
| `imu save` | keep the current configuration across resets |
| `imu reset` | return to the defaults and forget the saved configuration |

Everything the chip can do (filters, FIFO, interrupts, tap, free-fall, the
pedometer and so on) is reachable with `imu reg write`. Changes apply
immediately but last only until the next reset unless followed by `imu save`,
which stores the chip's FIFO, interrupt, control and tap/wake-up registers.
The `sleep` command takes the IMU over; the saved or default configuration is
restored when the board wakes.

### Sleep

`sleep` powers the board down to its lowest-power state. Bluetooth and USB
stop, and waking restarts the firmware.

- `sleep off` – sleep until the reset button is pressed. The IMU is powered
  down as well.
- `sleep motion [mg]` – sleep until the board is moved, or reset. The
  accelerometer stays on at 26 Hz in low-power mode and wakes the board when
  the acceleration changes by more than the threshold between two samples:
  32 to 1968 mg, 125 mg by default. Smaller values are more sensitive.

After a motion wake-up the log shows `Woken from sleep by motion`.

### Connecting

**BLE** – connect to **XIAO Sense Plus** with any Nordic UART Service terminal,
e.g. the UART feature in nRF Toolbox or the Bluefruit Connect app. From a
computer, use the included terminal (Ctrl-] quits):

```sh
pip install bleak
python tools/console.py
```

It connects to the first board it finds; pass a name
(`python tools/console.py "My Board"`) to pick one.

**Web app** – a browser page with the console and firmware upgrade side by
side, over one Bluetooth connection. It needs Chrome or Edge on a computer or
Android; Safari, Firefox and iOS have no Web Bluetooth.

Open <https://auli-ng.netlify.app>, click **Connect** and pick the board.

To run it locally instead:

```sh
python3 -m http.server -d web 8000
```

Then open <http://localhost:8000>. The page must be served from `localhost` or
over HTTPS; opening `index.html` as a file does not work.

The page is an installable app: use the install button in the browser's address
bar (or *Add to Home screen* on Android). Once installed it opens in its own
window and works offline. It bundles [xterm.js](https://xtermjs.org) (MIT) in
`web/vendor`.

The hosted copy is the Netlify project `auli-ng`, configured by
`netlify.toml`. It redeploys automatically on every push to `main`.

**USB** – open the board's CDC ACM serial port (`/dev/cu.usbmodem*` on macOS,
`/dev/ttyACM*` on Linux, a COM port on Windows) in any serial terminal, such as
the VS Code serial terminal or `screen`.

The firmware does not support pairing. If the board was previously bonded to
your phone or computer under other firmware, remove ("forget") that old device
entry first, or the connection is refused.

## OTA upgrade (BLE)

1. Bump the version in `VERSION` and rebuild.
2. Upload `build/dfu_application.zip` with the web app (see
   [Connecting](#connecting)) or with **nRF Connect Device Manager**
   (iOS/Android).

The board accepts one Bluetooth connection at a time, so disconnect any other
console session first.

MCUboot swaps the new image in on the next reset. The app confirms the image
once Bluetooth is up; an image that never gets that far is reverted on the
following reset.

## Signing key

MCUboot only boots and accepts OTA images signed with the key it was built
with. To use your own key instead of the public development one, point
`MCUBOOT_SIGNING_KEY` at an ECDSA P-256 private key when building:

```sh
# Once: generate a key. Keep it out of the repository and back it up.
python3 $ZEPHYR_BASE/../bootloader/mcuboot/scripts/imgtool.py keygen \
    -t ecdsa-p256 -k ~/.mcuboot/xiao-nrf52840-sense-plus.pem

MCUBOOT_SIGNING_KEY=~/.mcuboot/xiao-nrf52840-sense-plus.pem \
    west build -p -b xiao_ble/nrf52840/sense --sysbuild
```

The variable is read when the build is configured, so use a fresh build
directory (`-p`) when switching keys.

Changing the key changes the bootloader. A board must be flashed once over USB
with the new `first_flash.uf2`; after that it rejects OTA images signed with
any other key, and an OTA image signed with the new key is rejected by boards
still running the old bootloader. If the key is lost, deployed boards can only
be updated over USB.

For CI builds, the repository has the key as the GitHub Actions secret
`MCUBOOT_SIGNING_KEY` (the PEM text): write it to a file and set the variable
to that file's path.

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

- **Signing key** – by default images are signed with MCUboot's public
  development key, so anyone can sign an image the bootloader will accept. Build
  with your own key as described under [Signing key](#signing-key).
- **Open access** – the console and the OTA service accept unauthenticated,
  unencrypted connections from any device in range. Require pairing before
  deploying.

## License

[MIT](LICENSE)
