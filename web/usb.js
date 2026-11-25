// SPDX-License-Identifier: MIT
//
// Finds a board plugged in over USB and restarts it into the UF2 bootloader
// ("USB mode"), where it appears as the XIAO-SENSE drive that firmware is
// copied to. Works without Bluetooth, and for a board that has never run this
// firmware.
//
//   legacy Cato     (the z_cato firmware) is a USB HID device. It takes text
//                   commands in feature reports, one of which restarts it
//                   into the bootloader. Reached with WebHID.
//   this firmware   has a USB serial console whose shell has a "bootloader"
//                   command. Reached with Web Serial.
//   the bootloader  a board already in USB mode also has a serial port, with
//                   Seeed's USB IDs, and does not answer on it.

export const HID_SUPPORTED = typeof navigator !== "undefined" && "hid" in navigator;
export const SERIAL_SUPPORTED = typeof navigator !== "undefined" && "serial" in navigator;

export const KIND_CURRENT = "current";
export const KIND_LEGACY = "legacy";
export const KIND_BOOTLOADER = "bootloader";
export const KIND_UNKNOWN = "unknown";

export function describe(kind) {
	return {
		[KIND_CURRENT]: "a board running this firmware",
		[KIND_LEGACY]: "a legacy Cato",
		[KIND_BOOTLOADER]: "a board that is already in USB mode",
		[KIND_UNKNOWN]: "a device that did not answer like a Cato",
	}[kind];
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ----------------------------------------------------------- legacy Cato ---

const LEGACY_FILTERS = [{ vendorId: 0x1915, productId: 0x52dd }];
const LEGACY_REPORT_ID = 6; // its command channel
const LEGACY_REPORT_LENGTH = 244;
const LEGACY_FIRST_PACKET = 0;
const LEGACY_TO_BOOTLOADER = "reboot uf2";

/**
 * A legacy Cato command as its feature report: 0x00, 0xFF, the packet type,
 * the length of the text (16 bit, little endian), then the text, padded to
 * the report's fixed length.
 */
export function legacyCommandReport(command) {
	const text = new TextEncoder().encode(command);
	const report = new Uint8Array(LEGACY_REPORT_LENGTH);

	report.set([0x00, 0xff, LEGACY_FIRST_PACKET, text.length & 0xff, text.length >> 8]);
	report.set(text, 5);
	return report;
}

/** Legacy Catos this page has been given access to before and are plugged in. */
export async function knownLegacyDevices() {
	if (!HID_SUPPORTED) {
		return [];
	}
	return (await navigator.hid.getDevices()).filter((device) =>
		LEGACY_FILTERS.some((filter) =>
			device.vendorId === filter.vendorId && device.productId === filter.productId));
}

/**
 * Restarts a legacy Cato into USB mode. Uses one the page has been given
 * access to before, or else asks the user to pick it. Resolves false if none
 * was picked.
 */
export async function restartLegacyIntoUsbMode() {
	const known = await knownLegacyDevices();
	const [device] = known.length
		? known
		: await navigator.hid.requestDevice({ filters: LEGACY_FILTERS });

	if (!device) {
		return false;
	}
	if (!device.opened) {
		await device.open();
	}
	try {
		await device.sendFeatureReport(LEGACY_REPORT_ID, legacyCommandReport(LEGACY_TO_BOOTLOADER));
	} catch (err) {
		// It restarts a moment after the command and may never acknowledge it.
		if (err.name !== "NotAllowedError" && err.name !== "NetworkError") {
			throw err;
		}
	}
	await sleep(300);
	await device.close().catch(() => {});
	return true;
}

// --------------------------------------------- this firmware, bootloader ---

const SEEED_VID = 0x2886; // the UF2 bootloader
const ZEPHYR_VID = 0x2fe3; // this firmware's USB console
const SERIAL_FILTERS = [{ usbVendorId: ZEPHYR_VID }, { usbVendorId: SEEED_VID }];

const CLEAR_LINE = "\x15";

/** Works out what is on a serial port from what it sent back. */
export function classifySerial(text, vendorId) {
	if (/uart:~\$/.test(text)) {
		return KIND_CURRENT;
	}
	if (text.trim() === "" && vendorId === SEEED_VID) {
		return KIND_BOOTLOADER;
	}
	return KIND_UNKNOWN;
}

async function probe(port) {
	const encoder = new TextEncoder();
	const decoder = new TextDecoder();
	const writer = port.writable.getWriter();
	const reader = port.readable.getReader();
	let text = "";

	const pump = (async () => {
		try {
			for (;;) {
				const { value, done } = await reader.read();

				if (done) {
					break;
				}
				text += decoder.decode(value, { stream: true });
			}
		} catch {
			// The board reset, or the read was cancelled: either ends it.
		}
	})();

	try {
		// Enter draws the shell's prompt.
		await writer.write(encoder.encode(`${CLEAR_LINE}\r`));
		await sleep(700);

		const kind = classifySerial(text, port.getInfo().usbVendorId);

		if (kind === KIND_CURRENT) {
			await writer.write(encoder.encode(`${CLEAR_LINE}bootloader\r`));
			await sleep(300);
		}
		return kind;
	} finally {
		await reader.cancel().catch(() => {});
		await pump;
		reader.releaseLock();
		writer.releaseLock();
	}
}

/**
 * Asks the user to pick the board's serial port, works out what it is, and
 * restarts it into USB mode if it is not there already. Resolves with the
 * kind found; rejects with a NotFoundError if nothing was picked.
 */
export async function restartSerialIntoUsbMode() {
	const port = await navigator.serial.requestPort({ filters: SERIAL_FILTERS });

	await port.open({ baudRate: 115200 });
	try {
		return await probe(port);
	} finally {
		// Gone already if the board reset.
		await port.close().catch(() => {});
	}
}
