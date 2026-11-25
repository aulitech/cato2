# cato2

Firmware for the Cato, a
[Seeed Studio XIAO nRF52840 Sense Plus](https://wiki.seeedstudio.com/XIAO_BLE/),
built on Zephyr and the nRF Connect SDK, and **MyCato**, the web app that sets
it up, controls it and upgrades it (<https://auli-ng.netlify.app>, source in
`web/`).

- **Console** – the Zephyr shell and log output over the Nordic UART Service
  (NUS) and over USB serial
- **OTA upgrade** – MCUmgr/SMP over Bluetooth, with MCUboot swapping in the new
  image and reverting it if it fails to start
- **Pairing** – the Bluetooth console and OTA need an encrypted link; up to
  five paired devices are remembered and three can be connected at once
- **HID** – the board is a Bluetooth mouse and keyboard, and streams its
  accelerometer, gyroscope and fused orientation as HID sensor reports
- **No debug probe needed** – MCUboot is chain-loaded by the board's stock
  Adafruit UF2 bootloader, so the first flash is drag-and-drop, or one button
  in MyCato

The app blinks the RGB LED and adds shell commands to set the blink rate and
pattern (`blink`), rename the board (`name`), manage paired devices (`bonds`),
read and fully configure the on-board LSM6DS3TR-C IMU (`imu`), send mouse and
keyboard input and use the board as an air mouse (`hid`), sleep until reset
or motion (`sleep`), restart (`reboot`), return to factory defaults
(`factory_reset`), and restart into the USB bootloader (`bootloader`).

> **Verified on hardware** (XIAO nRF52840 Sense, UF2 bootloader 0.6.1): UF2 first
> flash, MCUboot chain-load, the shell over BLE and USB, every shell command
> below, pairing with a Mac, the HID input and property reports as seen by macOS, and OTA
> upgrades in both directions. Not exercised: MCUboot's automatic revert of a
> bad image, refusing a sixth or locked-out device, more than one device
> connected at once, and any host other than macOS (no phone, Windows or
> Linux). Also not yet tried on a real board: `factory_reset`, the erasing of a
> legacy Cato's stored data, how the air mouse feels in the hand, and MyCato's
> upgrade over USB (including of a legacy Cato), which has run only against a
> simulated board.

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
| `build/usb-upgrade.uf2` | MCUboot + signed app, for the first flash over USB |
| `build/cato.zip` | OTA update package |

## First flash (USB)

1. Double-tap the reset button. The board mounts as a USB drive named
   `XIAO-SENSE`.
2. Copy `build/usb-upgrade.uf2` onto the drive.

A board already running this firmware can skip the button: the `bootloader`
shell command resets it into the same USB drive mode.

The same procedure recovers a board at any time, since the UF2 bootloader is
left untouched.

MyCato does both steps with the published build: see
[Upgrading over USB](#upgrading-over-usb).

## Console

The Zephyr shell (with log output) is available over both Bluetooth and USB.
Type `help` for the command list. The prompt is the board's name.

```
Cato_1A2B:~$ accel
accel x=0.12 y=-0.34 z=9.79 m/s^2
```

If the firmware crashes it restarts by itself, and the next boot logs what
failed (`Restarted after a crash: ...`).

### Blink

The `blink` command shows or changes how the RGB LED blinks. Changes take
effect immediately and are kept across resets and OTA upgrades.

```
Cato_1A2B:~$ blink
rate 1000 ms, pattern rgb
Cato_1A2B:~$ blink rate 250
Cato_1A2B:~$ blink pattern r-b-w
```

- `blink rate <ms>` – time per pattern step, 20 to 60000 ms
- `blink pattern <steps>` – up to 32 steps, one character each: `r` `g` `b`
  red, green, blue; `y` `c` `m` yellow, cyan, magenta; `w` white; `-` off

### Name

`name` shows the board's Bluetooth name and `name <new name>` changes it, up
to 26 characters. Until it is given one, a board is called `Cato_` and the
last four digits of its serial number, such as `Cato_1A2B`. The name is also
the shell's prompt. The name is kept across resets. Phones and computers cache
names, so a board they have already seen may show its old name for a while.

### Restart, version and factory reset

`reboot` restarts the board; every setting is kept.

`version` shows the firmware version, from the `VERSION` file the build was
made with:

```
Cato_1A2B:~$ version
firmware 0.3.0
```

`factory_reset confirm` erases everything the board has stored – name, blink,
IMU and HID settings, the pairing switch and all paired devices – and restarts
it with the defaults. The firmware itself is not touched. Phones and computers
that were paired still hold their half of the pairing and cannot reconnect
until the board is removed ("forgotten") in their Bluetooth settings.

### IMU

`imu` shows the accelerometer and gyroscope configuration and `imu read` takes
one reading of acceleration, rotation rate and temperature (`accel` reads the
accelerometer alone).

```
Cato_1A2B:~$ imu
accel  odr 104 Hz, range +-2 g, high-performance mode
gyro   odr 104 Hz, range +-250 dps, high-performance mode
saved  no (defaults at boot)
Cato_1A2B:~$ imu accel range 8
Cato_1A2B:~$ imu gyro odr 0
Cato_1A2B:~$ imu save
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

### Pairing

The Bluetooth console and OTA service only work over an encrypted link, so a
phone or computer pairs with the board the first time it connects; most ask
you to confirm. Up to five paired devices (bonds) are remembered across resets
and upgrades. The USB console needs no pairing.

```
Cato_1A2B:~$ bonds
1  80:A9:97:09:B1:31 (public)  (connected)
1 of 5 bonds used, pairing on
Cato_1A2B:~$ bonds name 1 Office Mac
Cato_1A2B:~$ bonds
1  80:A9:97:09:B1:31 (public)  (connected)  "Office Mac"
1 of 5 bonds used, pairing on
```

- `bonds` – list the bonded devices
- `bonds name <number> [name]` – give a bonded device a name of up to 14
  characters, shown in the list in quotes; with no name, take it away. The
  name is kept on the board and goes when the bond is removed.
  A device with no name is asked for its own when it connects (its Bluetooth
  Device Name), and the first 14 plain characters of that become its name.
- `bonds remove <number|all>` – forget one device, or all of them
- `bonds pairing <on|off>` – accept or refuse new devices; kept across resets

The board has no display or buttons, so pairing only encrypts the link: it
cannot check who is pairing. Any device in range can pair while pairing is on
and a slot is free, so switch it off (`bonds pairing off`) once your own
devices are bonded. With all five slots in use, remove a bond before pairing
another device. Up to three devices can be connected at the same time; they
share the one Bluetooth shell.

If a bond is removed on one side only, the other side's connection attempts
fail: remove ("forget") the board on the phone or computer too, then pair
again. The same applies to a device that was bonded to the board under other
firmware.

### HID: mouse, keyboard and sensors

The board is a Bluetooth HID device. A paired computer or phone sees a mouse
and a keyboard, and keeps the connection open like any other Bluetooth mouse;
two more devices can still connect for the console and OTA.

| Report | Contents |
| --- | --- |
| 1 Mouse | five buttons, relative X and Y (16 bit), wheel |
| 2 Keyboard | modifiers and six keys; LED output report |
| 3 Accelerometer | HID Sensor *Accelerometer 3D*: X, Y, Z in thousandths of a g |
| 4 Gyroscope | HID Sensor *Gyrometer 3D*: X, Y, Z in tenths of a degree per second |
| 5 Orientation | HID Sensor *Device Orientation*: quaternion x, y, z, w, scaled by 10000 |

**Every report is off until you switch it on**, and the choice is kept across
resets:

```
Cato_1A2B:~$ hid enable mouse
Cato_1A2B:~$ hid enable orientation
Cato_1A2B:~$ hid
enabled: mouse orientation
hosts 1, listening to: mouse keyboard accel gyro orientation
sensor rate 50 Hz
keyboard LEDs 0x00
air mouse off, speed 20 20, accel 60 deg/s, dead zone 2 deg/s, axes auto, forward +y
link: interval 15.00 ms, 2M, MTU 498
Cato_1A2B:~$ hid mouse move 50 -20
```

- `hid enable <report|all>`, `hid disable <report|all>` – `mouse`, `keyboard`,
  `accel`, `gyro`, `orientation`
- `hid rate <Hz>` – rate of the sensor reports, 1 to 100 Hz (default 50)
- `hid mouse move <x> <y>`, `hid mouse click [left|right|middle]`,
  `hid mouse scroll <steps>`
- `hid air on`, `hid air off` – air mouse, see below
- `hid air speed <x> [y]`, `hid air accel <0-250>`, `hid air deadzone <0-20>`,
  `hid air forward <axis>`,
  `hid air axes auto` or `hid air axes <x> <y>`
- `hid key <key>` – tap one key: a character, a name (`enter` `esc` `tab`
  `space` `backspace` `delete` `insert` `home` `end` `pageup` `pagedown` `up`
  `down` `left` `right` `capslock` `f1`–`f12`), optionally with `ctrl` `shift`
  `alt` `gui` joined by `+`
- `hid type <text>` – type text, assuming a US keyboard layout on the host

`hid` also lists, for each connected device, the connection interval, radio
mode and MTU it negotiated.

**Air mouse** (called **Pointer** in MyCato, where it goes on and off with
the Mouse report). With the mouse report enabled and `hid air on`, turning the
board moves the pointer, like a presentation remote. A hundred times a second
the gyroscope's turn rates, with its zero offset removed, become relative
mouse movement:

- `hid air speed <x> [y]` – pointer counts per degree turned, 1 to 200,
  separately for sideways (X) and up-down (Y); one value sets both (default
  20 for each)
- `hid air accel <deg/s>` – quadratic acceleration: the pointer speed grows
  with the square of the turn rate, so slow turns are precise and fast ones
  cross the screen. The value is the turn rate at which the pointer moves at
  the set speed: at half that rate it moves at half the speed, at twice that
  rate at double (default 60). `0` turns acceleration off, and the pointer
  speed then simply follows the turn rate.
- `hid air deadzone` – turning slower than this many degrees per second is
  ignored, so tremor and noise do not move the pointer (default 2)

The pointer directions come from gravity (`hid air axes auto`, the default),
so they are right however the board is held or mounted, and stay right when
the wrist rolls: turning about the vertical moves the pointer sideways, and
tipping the board's forward axis up or down moves it up or down.

- `hid air forward <axis>` – the board axis that points at the screen, one of
  `+x -x +y -y +z -z` (default `+y`). This is the one thing gravity cannot
  tell. While that axis points within about 17 degrees of straight up or
  down, sideways has no meaning and the pointer holds still.
  In MyCato this is set on the **Orientation** card: a screen is drawn on the
  side of the board it is taken to be on, and clicking the dot on another
  side moves it there.
- `hid air axes <x> <y>` – fixed axes instead: the gyroscope axis that drives
  pointer X and the one that drives pointer Y, each with a sign. `-z -x` is
  what automatic gives for a board lying flat with forward `+y`.

The settings are kept across resets. The zero offset is measured when the
board has been still for a second, so let it rest briefly after power-up if
the pointer creeps. Buttons and the wheel still come from `hid mouse`.

**Orientation stream.** The fused orientation is also notified outside HID,
for applications that are not HID hosts: service
`7c3b0001-5f1e-4c8a-9d2e-3a6b8f1c2d4e`, characteristic `7c3b0002-…`, eight
bytes holding the quaternion x, y, z, w (board to room, Z up) as little-endian
16-bit integers scaled by 10000, 50 times a second while anything is
subscribed. It needs a paired link like everything else, and does not depend
on which HID reports are enabled. The web app's **Orientation** card uses it
to draw the board turning in 3D.

**Two HID services.** A HID over GATT report map is limited to 512 bytes, so
the mouse and keyboard are one HID service and the three sensors another.

**Sensor reports.** Each starts with the standard sensor state and event
bytes. Each sensor also has the HID Sensor property (feature) report that
operating system sensor drivers use: connection type, reporting state, power
state, sensor state, report interval and change sensitivity. A host can pace a
sensor by writing the report interval (one rate serves all three sensors) and
stop or restart its reports with the reporting or power state. A report is
sent only if it is enabled on the board and the host has not turned it off.
Maximum and minimum properties are left out to fit the size limit.

The enabled sensor reports are streamed while a host is connected. The IMU is
sampled at 100 Hz with whatever rate and range `imu` has set, so keep both
sensors on at 104 Hz or more for a meaningful orientation. The orientation
combines the gyroscope with the accelerometer's sense of down, and removes the
gyroscope's zero offset whenever the board has been still for a second. There
is no magnetometer, so heading is relative to where the board was when
streaming started and drifts slowly.

**Throughput.** The board asks for the 2 Mbit radio mode, a 498-byte MTU and
an 11.25 ms connection interval, queues several reports per connection event,
and packs reports into one packet for hosts that support multiple-value
notifications. What gets through is up to the host. macOS kept a 15 ms
interval and took about 120 reports per second in total: one sensor at 100 Hz,
or all three at about 40 Hz each.

The Device Information Service carries the chip's unique ID as the serial
number, and the pid.codes test IDs (`1209:0001`) as vendor and product:
replace those in `prj.conf` with your own.

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

**BLE** – connect to the board (**Cato_xxxx** until renamed) with any Nordic UART Service terminal,
e.g. the UART feature in nRF Toolbox or the Bluefruit Connect app. From a
computer, use the included terminal (Ctrl-] quits):

```sh
pip install bleak
python tools/console.py
```

It connects to the first board it finds; pass a name
(`python tools/console.py "My Board"`) to pick one.

**MyCato, the web app** – a browser page with the console, a tab of controls
for every command above (name, restart, factory reset, blink, IMU, paired
devices, sleep, mouse and pointer, keyboard, bootloader), a live 3D view of
the board's orientation and firmware upgrade, all over one Bluetooth
connection. The
controls run the same shell commands you would type, visibly in the console.
It needs Chrome or Edge on a computer or Android; Safari, Firefox and iOS have
no Web Bluetooth.

Open <https://auli-ng.netlify.app>, click **Connect** and pick the board.

If the connection drops for any reason but **Disconnect** (the board
restarted, slept, went out of range), the app reconnects by itself as soon as
the board is back. After a reload it also reconnects to the board it was last
connected to, in browsers that let a page see the Bluetooth devices it was
given before (`navigator.bluetooth.getDevices`); elsewhere, press **Connect**.

The page is laid out as tabs, one for each part of the board (General, Blink,
IMU, Paired devices, Sleep, Mouse and keyboard, Firmware upgrade). One shows
at a time, and dragging a tab sideways changes the order. The Orientation
view is not a tab: it stays in the top left corner, beside the tabs, and
opens and closes by its heading.

The console is a drawer: the buttons in its heading dock it left, at the
bottom or right, open it in a window of its own, or hide it. **Console** at
the top of the page brings it back, and the bar between drawer and tabs
resizes it. In a narrow window the drawer is always at the bottom. The order
of the tabs, the tab showing and the place and size of the drawer are
remembered in the browser.

The menu at the right end of the top row opens **Settings** (light or dark
appearance, console text size, resetting the arrangement, forgetting USB
drives), **Help** and **About**, which shows the app's version, the board's
firmware version and the latest published one.

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

## Firmware upgrade

MyCato's **Firmware upgrade** card says whether the board already has the most
recent build of `main` (see [Published builds](#published-builds)) and
installs it with one button: **Upgrade over Bluetooth**, or **Upgrade over
USB** for a new board or one that does not accept the Bluetooth upgrade.
Settings and paired devices are kept either way, except on a legacy Cato (below).

### Upgrading over Bluetooth

**Upgrade over Bluetooth** sends the published build to the connected board
and resets it. MCUboot swaps the new image in on that reset. The firmware
confirms the image once Bluetooth is up; an image that never gets that far is
reverted on the following reset.

For your own build, upload `build/cato.zip` under **Advanced** in the same
card, or with **nRF Connect Device Manager** (iOS/Android). The board accepts
it only if it is signed with the key its bootloader has (see
[Signing key](#signing-key)).

Up to three devices can be connected at once, so a console session or a
computer using the board as a mouse does not block the upgrade.

### Upgrading over USB

**Upgrade over USB** does the whole upgrade from the page: it restarts the
board into the bootloader, where it is a drive named `XIAO-SENSE` (or
`AULI-UPDAT` on some legacy Catos), downloads `usb-upgrade.uf2`, and writes it
onto that drive. The board installs the file and restarts.

The browser makes the user grant access to each thing once, so the first
upgrade has two choices to make; later ones need none, or at most a
confirmation that the page may write to the drive:

- **the board**, to restart it. A board connected over Bluetooth is restarted
  over that connection. Otherwise press **Board with this firmware** (reached
  through its USB console) or **Legacy Cato**, and pick it from the browser's
  list. If the board does not switch, the page says so and waits: double-tap
  the board's reset button, which does the same with no browser involved.
- **the drive**, with **Choose the board's drive**. The page checks that
  the folder chosen is a board's drive (it holds `INFO_UF2.TXT`) before it
  writes anything. On macOS the system also asks once whether the browser may
  use removable volumes.

This needs Chrome or Edge on a computer. In other browsers, **Do it by hand**
gives the file to download and copy onto the drive yourself.

### Upgrading a legacy Cato

A Cato running the earlier firmware ([z_cato](https://github.com/aulitech/z_cato))
is upgraded the same way. Plug it in, press **Upgrade over USB**, then
**Legacy Cato**: the page finds it as a USB HID device (`1915:52DD`) and sends
it its own `reboot uf2` command, which brings it back as the `XIAO-SENSE` or
`AULI-UPDAT` drive. Once the browser has been given access to a legacy Cato,
the card notices it whenever it is plugged in, and **Upgrade over USB**
restarts it without asking.

On its first start after such an upgrade, this firmware finds the earlier
firmware's data where its settings belong, with none of its own in it, and
erases that area once. The board starts with default settings and no paired
devices, so remove ("forget") the old Cato on phones and computers and pair
again.

## Published builds

Every push to `main` that changes the firmware is built by GitHub Actions
(`.github/workflows/firmware.yml`) with the project's signing key and published
as the [`firmware-latest`](https://github.com/aulitech/cato2/releases/tag/firmware-latest)
release:

| File | Use |
| --- | --- |
| `usb-upgrade.uf2` | First flash over USB |
| `cato.zip` | Upgrade over Bluetooth |
| `firmware.json` | Version, commit and build time |

The same files are the only commit on the `firmware` branch, which is where the
web app reads them from. A board accepts the published upgrade only if its
bootloader has the project's key, that is, if it was last flashed over USB with
a published `usb-upgrade.uf2`. A board flashed from a local build without the
key needs that USB flash once.

## Branches and versions

Work is committed to `preview`, the repository's default branch, and reaches
`main` by pull request. Firmware is built and published, and the hosted
MyCato deployed, from `main`.

The firmware and the web app have separate versions, and each changes only
when its own files do:

| | Version in | Changes with |
| --- | --- | --- |
| Firmware | `VERSION` | everything that goes into the build (`src`, `dts`, `sysbuild`, the configuration files) |
| MyCato | `web/version.js` | the files under `web/` |

The patch number goes up with every commit to `preview` that changes that
part, and the minor number with every pull request to `main` (the major
number instead, when the change calls for it). The firmware's version is what
`firmware.json` and the Firmware upgrade card report; MyCato's is shown under
**About** and names the cache of the installed app, so a new version replaces
the old one's files.

## Signing key

MCUboot only boots and accepts OTA images signed with the key it was built
with. To use your own key instead of the public development one, point
`MCUBOOT_SIGNING_KEY` at an ECDSA P-256 private key when building:

```sh
# Once: generate a key. Keep it out of the repository and back it up.
python3 $ZEPHYR_BASE/../bootloader/mcuboot/scripts/imgtool.py keygen \
    -t ecdsa-p256 -k ~/.mcuboot/cato2.pem

MCUBOOT_SIGNING_KEY=~/.mcuboot/cato2.pem \
    west build -p -b xiao_ble/nrf52840/sense --sysbuild
```

A key at exactly that path, `~/.mcuboot/cato2.pem`, is used
without setting the variable, so on a machine that has the project's key every
build is signed with it. The key is chosen when the build is configured, so use
a fresh build directory (`-p`) when switching keys.

Changing the key changes the bootloader. A board must be flashed once over USB
with the new `usb-upgrade.uf2`; after that it rejects OTA images signed with
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
- **Pairing is unauthenticated** – links are encrypted, but any device in
  range can pair while pairing is on (see [Pairing](#pairing)). Switch pairing
  off after setting up, or add a passkey or out-of-band step if the product
  can support one.
- **USB console is open** – anyone with physical access gets the shell,
  including `bonds`, `factory_reset` and `bootloader`.

## License

[MIT](LICENSE)
