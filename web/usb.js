// SPDX-License-Identifier: MIT
//
// Finds a board plugged in over USB, restarts it into the UF2 bootloader
// ("USB mode"), where it appears as a drive (XIAO-SENSE, or AULI-UPDAT on some
// legacy Catos), and writes firmware onto that drive. Works without Bluetooth, and for a board that has never
// run this firmware.
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

async function restartPort(port) {
	await port.open({ baudRate: 115200 });
	try {
		return await probe(port);
	} finally {
		// Gone already if the board reset.
		await port.close().catch(() => {});
	}
}

/**
 * Asks the user to pick the board's serial port, works out what it is, and
 * restarts it into USB mode if it is not there already. Resolves with the
 * kind found; rejects with a NotFoundError if nothing was picked.
 */
export async function restartSerialIntoUsbMode() {
	return restartPort(await navigator.serial.requestPort({ filters: SERIAL_FILTERS }));
}

/**
 * Restarts a board into USB mode without asking anything, which works for one
 * the page has been given access to before. Resolves with the kind found, or
 * null if no such board is plugged in.
 */
export async function restartKnownIntoUsbMode() {
	if ((await knownLegacyDevices().catch(() => [])).length) {
		await restartLegacyIntoUsbMode();
		return KIND_LEGACY;
	}
	if (!SERIAL_SUPPORTED) {
		return null;
	}

	const ports = (await navigator.serial.getPorts()).filter((port) =>
		SERIAL_FILTERS.some((filter) => filter.usbVendorId === port.getInfo().usbVendorId));

	for (const port of ports) {
		try {
			const kind = await restartPort(port);

			if (kind !== KIND_UNKNOWN) {
				return kind;
			}
		} catch {
			// In use elsewhere, or unplugged: try the next.
		}
	}
	return null;
}

// -------------------------------------------------------------- the drive ---
//
// In USB mode the board is a drive, and a UF2 file written to it is installed.
// The File System Access API lets the page do that writing once the user has
// pointed it at the drive. Drives are remembered by name, so that later
// upgrades need at most a confirmation.

/** What the drive is called, for telling the user what to look for. */
export const DRIVE_NAMES = "XIAO-SENSE or AULI-UPDAT";

export const DRIVE_SUPPORTED = typeof window !== "undefined" && "showDirectoryPicker" in window;

const DRIVE_MARKER = "INFO_UF2.TXT"; // every UF2 bootloader's drive has it
const UF2_BLOCK = 512;
const UF2_MAGIC = 0x0a324655; // "UF2\n"
const WRITE_CHUNK = 64 * UF2_BLOCK;
const POLL_MS = 500;

const DB_NAME = "cato-usb";
const DB_STORE = "handles";

function withStore(mode, action) {
	return new Promise((resolve, reject) => {
		const open = indexedDB.open(DB_NAME, 1);

		open.onupgradeneeded = () => open.result.createObjectStore(DB_STORE);
		open.onerror = () => reject(open.error);
		open.onsuccess = () => {
			const db = open.result;
			const transaction = db.transaction(DB_STORE, mode);
			let request;

			try {
				request = action(transaction.objectStore(DB_STORE));
			} catch (err) {
				db.close();
				reject(err);
				return;
			}

			transaction.oncomplete = () => {
				db.close();
				resolve(request.result);
			};
			transaction.onerror = () => {
				db.close();
				reject(transaction.error);
			};
		};
	});
}

/** The drives chosen on earlier visits. */
export async function rememberedDrives() {
	if (!DRIVE_SUPPORTED) {
		return [];
	}
	try {
		return (await withStore("readonly", (store) => store.getAll())) || [];
	} catch {
		return [];
	}
}

/** Forgets every remembered drive. */
export function forgetDrives() {
	return withStore("readwrite", (store) => store.clear()).catch(() => {});
}

/** Whether a UF2 file is well formed: whole blocks, each with the magic number. */
export function isUf2(bytes) {
	if (bytes.length === 0 || bytes.length % UF2_BLOCK !== 0) {
		return false;
	}

	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

	for (let offset = 0; offset < bytes.length; offset += UF2_BLOCK) {
		if (view.getUint32(offset, true) !== UF2_MAGIC) {
			return false;
		}
	}
	return true;
}

/** Whether the folder is a board's drive, mounted right now. */
export async function isBootloaderDrive(drive) {
	try {
		const file = await (await drive.getFileHandle(DRIVE_MARKER)).getFile();

		return /UF2/i.test(await file.text());
	} catch {
		return false;
	}
}

/**
 * Whether the page may write to a remembered drive. With ask, the browser
 * asks the user if it has to, which only works straight after a click.
 */
export async function mayWriteDrive(drive, ask = false) {
	const options = { mode: "readwrite" };

	try {
		if ((await drive.queryPermission(options)) === "granted") {
			return true;
		}
		return ask && (await drive.requestPermission(options)) === "granted";
	} catch {
		return false;
	}
}

/**
 * Asks the user to choose the board's drive and remembers it. Rejects with an
 * AbortError if nothing was chosen and a NotFoundError if what was chosen is
 * not a board's drive.
 */
export async function chooseDrive() {
	const drive = await window.showDirectoryPicker({ id: "cato-drive", mode: "readwrite" });

	if (!(await isBootloaderDrive(drive))) {
		throw new DOMException("Not the board's drive", "NotFoundError");
	}
	await withStore("readwrite", (store) => store.put(drive, drive.name)).catch(() => {});
	return drive;
}

/** The first of the drives that is mounted right now, or null. */
export async function mountedDrive(drives) {
	for (const drive of drives) {
		if (await isBootloaderDrive(drive)) {
			return drive;
		}
	}
	return null;
}

/** Waits for one of the drives to be mounted and resolves with it, or null. */
export async function waitForDrive(drives, timeout) {
	const deadline = Date.now() + timeout;

	for (;;) {
		const drive = await mountedDrive(drives);

		if (drive || Date.now() >= deadline) {
			return drive;
		}
		await sleep(POLL_MS);
	}
}

async function waitForDriveGone(drive, timeout) {
	const deadline = Date.now() + timeout;

	while (await isBootloaderDrive(drive)) {
		if (Date.now() >= deadline) {
			return false;
		}
		await sleep(POLL_MS);
	}
	return true;
}

/**
 * Writes a UF2 file onto the drive. The board installs it and restarts as
 * soon as it has the last block, taking the drive away, so the tail end of
 * the write commonly fails: what counts is that the drive is gone afterwards.
 * Resolves true if the board took the file.
 */
export async function installOnDrive(drive, name, bytes, onProgress = () => {}) {
	try {
		const file = await drive.getFileHandle(name, { create: true });
		const writable = await file.createWritable();

		for (let offset = 0; offset < bytes.length; offset += WRITE_CHUNK) {
			await writable.write(bytes.subarray(offset, offset + WRITE_CHUNK));
			onProgress(Math.min(offset + WRITE_CHUNK, bytes.length) / bytes.length);
		}
		await writable.close();
	} catch (err) {
		if (await isBootloaderDrive(drive)) {
			throw err;
		}
	}
	return waitForDriveGone(drive, 15000);
}
