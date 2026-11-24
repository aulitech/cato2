#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Interactive terminal for the board's BLE shell (Nordic UART Service).

    pip install bleak
    python tools/console.py [device-name]

Without a name it connects to the first board found that offers the UART
service, whatever it has been renamed to.

Keys are sent as typed, so tab completion and history work as on a serial
terminal. Press Ctrl-] to quit.
"""

import asyncio
import os
import sys

from bleak import BleakClient, BleakScanner

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host -> board
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # board -> host
QUIT_KEY = b"\x1d"  # Ctrl-]


async def run(name: str | None) -> int:
    if name:
        print(f"Scanning for '{name}'...", file=sys.stderr)
        device = await BleakScanner.find_device_by_name(name, timeout=15)
    else:
        print("Scanning for a board...", file=sys.stderr)
        device = await BleakScanner.find_device_by_filter(
            lambda _, adv: NUS_SERVICE in adv.service_uuids, timeout=15)
    if device is None:
        print("No board found. Is it powered, awake and not connected elsewhere?",
              file=sys.stderr)
        return 1

    loop = asyncio.get_running_loop()
    done = asyncio.Event()
    keys: asyncio.Queue[bytes] = asyncio.Queue()
    stdin_fd = sys.stdin.fileno()
    interactive = sys.stdin.isatty()

    def on_output(_, data: bytearray) -> None:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    def on_key() -> None:
        data = os.read(stdin_fd, 256)
        if not data or QUIT_KEY in data:
            loop.remove_reader(stdin_fd)
            keys.put_nowait(b"")
        else:
            keys.put_nowait(data)

    async with BleakClient(device, disconnected_callback=lambda _: done.set()) as client:
        await client.start_notify(NUS_TX, on_output)
        print(f"Connected to {device.name or device.address}. Ctrl-] to quit.",
              file=sys.stderr)
        chunk = client.mtu_size - 3

        saved_tty = None
        if interactive:
            import termios
            import tty

            saved_tty = termios.tcgetattr(stdin_fd)
            tty.setraw(stdin_fd)
        try:
            loop.add_reader(stdin_fd, on_key)
            # Draw a prompt.
            await client.write_gatt_char(NUS_RX, b"\r", response=False)

            while not done.is_set():
                get_key = asyncio.ensure_future(keys.get())
                wait_done = asyncio.ensure_future(done.wait())
                await asyncio.wait({get_key, wait_done},
                                   return_when=asyncio.FIRST_COMPLETED)
                wait_done.cancel()
                if not get_key.done():
                    get_key.cancel()
                    break
                data = get_key.result()
                if not data:
                    break
                if not interactive:
                    data = data.replace(b"\n", b"\r")
                for i in range(0, len(data), chunk):
                    await client.write_gatt_char(NUS_RX, data[i:i + chunk],
                                                 response=False)

            if not interactive:
                # Piped input: give the board a moment to answer.
                await asyncio.sleep(1.5)
        finally:
            loop.remove_reader(stdin_fd)
            if saved_tty is not None:
                termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved_tty)

    print("\nDisconnected.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(run(sys.argv[1] if len(sys.argv) > 1 else None)))
