// SPDX-License-Identifier: MIT
//
// BLE console (Nordic UART Service) and OTA upgrade (MCUmgr SMP) over one
// Web Bluetooth connection.

import { initControls } from "./controls.js";
import { initOrientation, ORIENTATION_SERVICE, ORIENTATION_VALUE } from "./orientation.js";
import { ShellRunner } from "./shell.js";
import * as smp from "./smp.js";
import * as usb from "./usb.js";
import { initLayout } from "./layout.js";
import { initMenu } from "./menu.js";

const NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // browser -> board
const NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // board -> browser
const SMP_SERVICE = "8d53dc1d-1db7-4cd3-868b-8a527460aa84";
const SMP_CHAR = "da2e7828-fbce-4e01-ae9e-261174997c48";

// Largest write that fits the 247-byte ATT MTU desktop browsers negotiate.
// If the link turns out to have a smaller MTU, writes fall back to SMALL_WRITE.
const LARGE_WRITE = 244;
const SMALL_WRITE = 20;

const SMP_TIMEOUT_MS = 10000;
const REBOOT_TIMEOUT_MS = 120000;

const $ = (id) => document.getElementById(id);

// Web Bluetooth is missing in Safari, Firefox and every browser on iOS.
const BLUETOOTH = Boolean(navigator.bluetooth);

const ui = {
	unsupported: $("unsupported"),
	connect: $("connect"),
	status: $("status"),
	deviceName: $("device-name"),
	terminal: $("terminal"),
	clear: $("clear"),
	slots: $("slots"),
	refresh: $("refresh"),
	file: $("file"),
	fileInfo: $("file-info"),
	upload: $("upload"),
	firmwareBadge: $("firmware-badge"),
	firmwareStatus: $("firmware-status"),
	firmwareLatest: $("firmware-latest"),
	upgradeBle: $("upgrade-ble"),
	upgradeUsb: $("upgrade-usb"),
	usbPanel: $("usb-panel"),
	usbChoices: $("usb-choices"),
	usbDrive: $("usb-drive"),
	usbManual: $("usb-manual"),
	usbDownload: $("usb-download"),
	usbFindLegacy: $("usb-find-legacy"),
	usbFind: $("usb-find"),
	usbResult: $("usb-result"),
	upgradeProgress: $("upgrade-progress"),
	progress: $("progress"),
	progressText: $("progress-text"),
	log: $("log"),
};

const state = {
	device: null,
	nusRx: null,
	smpChar: null,
	writeSize: LARGE_WRITE,
	seq: 0,
	pending: null, // { seq, resolve, reject, timer }
	firmware: null, // a file chosen under Advanced
	latest: null, // the published build: { info, firmware }, see loadLatest()
	latestFailed: false,
	images: [], // what the board last reported
	legacyOnUsb: false, // a legacy Cato this page knows is plugged in
	busy: false, // an upgrade is running
	expectDisconnect: false,
};

const assembler = new smp.PacketAssembler();
const decoder = new TextDecoder();
const encoder = new TextEncoder();

// ------------------------------------------------------------- Terminal ----

const term = new Terminal({
	convertEol: false,
	cursorBlink: true,
	fontFamily: 'ui-monospace, "SF Mono", Menlo, Consolas, monospace',
	fontSize: 13,
	scrollback: 5000,
	theme: { background: "#11151a", foreground: "#d7dde5", cursor: "#d7dde5" },
});
const fit = new FitAddon.FitAddon();

term.loadAddon(fit);
term.open(ui.terminal);
fit.fit();
new ResizeObserver(() => fit.fit()).observe(ui.terminal);

// Typed in the console, here or in its popped-out window.
function sendInput(data) {
	if (state.nusRx) {
		write(state.nusRx, encoder.encode(data)).catch(() => {});
	}
}

term.onData(sendInput);

// Cards and the console drawer; told of all output for the popped-out console.
const layout = initLayout({ term, sendInput });

// Settings, Help and About, behind the menu in the top row.
initMenu({
	term,
	refit: () => fit.fit(),
	about: () => {
		const info = state.latest && state.latest.info;
		const running = state.smpChar ? state.images.find((image) => image.slot === 0) : null;
		const built = info ? new Date(info.built) : null;
		const can = (yes) => (yes ? "yes" : "not in this browser");

		return [
			["MyCato", `version ${self.MYCATO_VERSION}`],
			["Board", state.smpChar
				? `${state.device.name || "Unnamed"}, firmware ${running ? running.version : "unknown"}`
				: "Not connected"],
			["Latest firmware", info
				? `${info.version} (${String(info.commit).slice(0, 7)})` +
				  (built && !Number.isNaN(built.getTime())
					? `, built ${built.toLocaleDateString(undefined, { dateStyle: "medium" })}`
					: "")
				: "Not known"],
			["Bluetooth", can(BLUETOOTH)],
			["Upgrade over USB", can(usb.DRIVE_SUPPORTED)],
		];
	},
});

function consoleWrite(data) {
	term.write(data);
	layout.output(data);
}

// The control cards run shell commands over the same stream as the console.
const shell = new ShellRunner({
	send: (text) => {
		if (!state.nusRx) {
			return Promise.reject(new Error("Not connected"));
		}
		return write(state.nusRx, encoder.encode(text));
	},
	display: consoleWrite,
});

const orientation = initOrientation();
const controls = initControls({
	shell,
	onName: (name) => {
		if (name) {
			ui.deviceName.textContent = name;
		}
	},
});

function termNote(text) {
	consoleWrite(`\r\n\x1b[2m-- ${text} --\x1b[0m\r\n`);
}

// ------------------------------------------------------------------ GATT ---

// Web Bluetooth allows one GATT operation at a time per device, so every
// write from the console and from SMP goes through this queue.
let gattQueue = Promise.resolve();

function enqueue(operation) {
	const result = gattQueue.then(operation);

	gattQueue = result.catch(() => {});
	return result;
}

function write(characteristic, bytes) {
	return enqueue(async () => {
		let offset = 0;

		while (offset < bytes.length) {
			const chunk = bytes.subarray(offset, offset + state.writeSize);

			try {
				await characteristic.writeValueWithoutResponse(chunk);
			} catch (err) {
				if (state.writeSize === SMALL_WRITE || !state.device?.gatt.connected) {
					throw err;
				}
				// Most likely larger than this link's MTU: retry in small pieces.
				state.writeSize = SMALL_WRITE;
				log("Link has a small MTU; using 20-byte writes (slower).");
				continue;
			}
			offset += chunk.length;
		}
	});
}

async function openServices() {
	const server = await state.device.gatt.connect();

	const nus = await server.getPrimaryService(NUS_SERVICE);
	const nusTx = await nus.getCharacteristic(NUS_TX);

	state.nusRx = await nus.getCharacteristic(NUS_RX);
	nusTx.addEventListener("characteristicvaluechanged", (event) => {
		const value = event.target.value;

		// slice(): the runner may keep the bytes past this event.
		shell.feed(new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice());
	});
	await nusTx.startNotifications();

	const smpService = await server.getPrimaryService(SMP_SERVICE);

	state.smpChar = await smpService.getCharacteristic(SMP_CHAR);
	state.smpChar.addEventListener("characteristicvaluechanged", onSmpNotification);
	await state.smpChar.startNotifications();

	// Not in older firmware: the viewer then says so.
	let orientationValue = null;

	try {
		const service = await server.getPrimaryService(ORIENTATION_SERVICE);

		orientationValue = await service.getCharacteristic(ORIENTATION_VALUE);
	} catch {
		// no orientation stream
	}
	orientation.setSource(orientationValue, true);

	assembler.reset();
	state.writeSize = LARGE_WRITE;
}

async function connect() {
	const device = await navigator.bluetooth.requestDevice({
		filters: [{ services: [NUS_SERVICE] }, { namePrefix: "XIAO" }],
		optionalServices: [NUS_SERVICE, SMP_SERVICE, ORIENTATION_SERVICE],
	});

	if (state.device && state.device !== device) {
		state.device.removeEventListener("gattserverdisconnected", onDisconnected);
	}
	state.device = device;
	device.addEventListener("gattserverdisconnected", onDisconnected);

	setStatus("connecting", "Connecting…");
	await openServices();
	onConnected();
}

function onConnected() {
	setStatus("connected", "Connected");
	ui.deviceName.textContent = state.device.name || "Unnamed device";
	termNote(`connected to ${state.device.name || "device"}`);
	updateControls();
	// The first command also draws the shell prompt.
	controls.refresh();
	if (!state.busy) {
		refreshSlots().catch((err) => log(`Could not read image list: ${err.message}`));
	}
}

function onDisconnected() {
	orientation.setSource(null);
	queueMicrotask(showFirmwareStatus);
	state.nusRx = null;
	state.smpChar = null;
	shell.reset();
	if (state.pending) {
		clearTimeout(state.pending.timer);
		state.pending.reject(new Error("Disconnected"));
		state.pending = null;
	}
	setStatus("disconnected", state.expectDisconnect ? "Rebooting…" : "Disconnected");
	termNote("disconnected");
	updateControls();
}

function disconnect() {
	state.device?.gatt.disconnect();
}

// ------------------------------------------------------------------- SMP ---

function onSmpNotification(event) {
	const value = event.target.value;
	let packet;

	try {
		packet = assembler.push(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
	} catch (err) {
		assembler.reset();
		if (state.pending) {
			clearTimeout(state.pending.timer);
			state.pending.reject(new Error(`Bad response: ${err.message}`));
			state.pending = null;
		}
		return;
	}

	if (packet && state.pending && packet.seq === state.pending.seq) {
		clearTimeout(state.pending.timer);
		state.pending.resolve(packet.payload);
		state.pending = null;
	}
}

// One request at a time: each waits for the previous response.
let smpQueue = Promise.resolve();

function sendPacket(seq, packet) {
	const run = async () => {
		if (!state.smpChar) {
			throw new Error("Not connected");
		}

		const response = new Promise((resolve, reject) => {
			const timer = setTimeout(() => {
				state.pending = null;
				reject(new Error("No response from the board"));
			}, SMP_TIMEOUT_MS);

			state.pending = { seq, resolve, reject, timer };
		});

		// Awaited below; this only stops an early rejection being reported
		// as unhandled while the write is still in flight.
		response.catch(() => {});

		assembler.reset();
		try {
			await write(state.smpChar, packet);
		} catch (err) {
			if (state.pending) {
				clearTimeout(state.pending.timer);
				state.pending = null;
			}
			throw err;
		}

		const payload = await response;
		const rc = smp.responseError(payload);

		if (rc) {
			throw new Error(`Board refused the request: ${smp.describeError(rc)}`);
		}
		return payload;
	};

	const result = smpQueue.then(run);

	smpQueue = result.catch(() => {});
	return result;
}

function nextSeq() {
	state.seq = (state.seq + 1) & 0xff;
	return state.seq;
}

function request(op, group, id, payload) {
	const seq = nextSeq();

	return sendPacket(seq, smp.encodePacket(op, group, id, seq, payload));
}

async function readSlots() {
	const payload = await request(smp.OP_READ, smp.GROUP_IMAGE, smp.IMAGE_STATE);

	return payload.images || [];
}

// ------------------------------------------------------------------ OTA ----

function renderSlots(images) {
	ui.slots.replaceChildren();

	if (!images.length) {
		const row = ui.slots.insertRow();

		row.insertCell().textContent = "No images reported";
		row.cells[0].colSpan = 4;
		return;
	}

	for (const image of images) {
		const row = ui.slots.insertRow();
		const flags = ["active", "confirmed", "pending"].filter((flag) => image[flag]);

		row.insertCell().textContent = image.slot === 0 ? "Running" : "Standby";
		row.insertCell().textContent = image.version;
		row.insertCell().textContent = image.hash ? smp.toHex(image.hash).slice(0, 16) : "–";
		row.insertCell().textContent = flags.join(", ") || "–";
	}
}

async function refreshSlots() {
	const images = await readSlots();

	state.images = images;
	renderSlots(images);
	showFirmwareStatus();
	return images;
}

/** Makes a firmware file the one to upload; get() resolves to its bytes. */
async function selectFirmware(name, get, source = "") {
	state.firmware = null;
	ui.fileInfo.textContent = "";
	ui.fileInfo.classList.remove("error");

	try {
		state.firmware = await smp.loadFirmware(name, await get());
		ui.fileInfo.textContent =
			`${source}${state.firmware.name} · version ${state.firmware.version} · ` +
			`${state.firmware.size.toLocaleString()} bytes`;
	} catch (err) {
		ui.fileInfo.textContent = err.message;
		ui.fileInfo.classList.add("error");
	}
	updateControls();
}

async function chooseFile() {
	const file = ui.file.files[0];

	if (file) {
		await selectFirmware(file.name, async () => new Uint8Array(await file.arrayBuffer()));
	} else {
		state.firmware = null;
		ui.fileInfo.textContent = "";
		updateControls();
	}
}

// The Firmware workflow (.github/workflows/firmware.yml) publishes each build
// of main twice: on the "firmware" branch, which this page can read from
// another origin, and as a release, which gives a proper file download.
// firmware.json describes the build.
const REPO = "aulitech/xiao-nrf52840-sense-plus";
const FIRMWARE_DIR = `https://raw.githubusercontent.com/${REPO}/firmware/`;
const RELEASE_DIR = `https://github.com/${REPO}/releases/download/firmware-latest/`;

async function fetchFirmwareFile(name) {
	const response = await fetch(FIRMWARE_DIR + name, { cache: "no-cache" });

	if (!response.ok) {
		throw new Error(`Could not download ${name} (${response.status})`);
	}
	return response;
}

// Fetches the published build once: its description, and the Bluetooth
// upgrade package, which also tells whether the board already runs it.
async function loadLatest() {
	try {
		const info = await (await fetchFirmwareFile("firmware.json")).json();

		if (!info.dfu || !info.usb) {
			throw new Error("Incomplete firmware description");
		}

		const bytes = new Uint8Array(await (await fetchFirmwareFile(info.dfu)).arrayBuffer());

		state.latest = { info, firmware: await smp.loadFirmware(info.dfu, bytes) };
		ui.usbDownload.href = RELEASE_DIR + info.usb;
	} catch {
		state.latestFailed = true;
	}
	showFirmwareStatus();
	updateControls();
}

/** One line on where the board stands against the latest build. */
function showFirmwareStatus() {
	const latest = state.latest;
	const running = state.smpChar ? state.images.find((image) => image.slot === 0) : null;
	let text;
	let kind = "";
	let badge = "";

	if (!latest) {
		text = state.latestFailed
			? "The latest firmware could not be downloaded. Check the internet connection and reload."
			: "Checking for the latest firmware…";
	} else if (state.legacyOnUsb && !running) {
		text = "A legacy Cato is connected by USB. Upgrade it over USB.";
		kind = "update";
		badge = "Legacy Cato found";
	} else if (!BLUETOOTH) {
		text = "Upgrade the board over USB.";
	} else if (!running) {
		text = "Connect to check the board's firmware, or upgrade it over USB.";
	} else if (running.hash && latest.firmware.hash &&
		   smp.toHex(running.hash) === smp.toHex(latest.firmware.hash)) {
		text = "The board has the latest firmware.";
		kind = "ok";
		badge = "Up to date";
	} else {
		text = "A newer firmware is available.";
		kind = "update";
		badge = "Update available";
	}

	ui.firmwareStatus.textContent = text;
	ui.firmwareStatus.className = kind;
	ui.firmwareBadge.textContent = badge;

	if (latest) {
		const built = new Date(latest.info.built);
		const when = Number.isNaN(built.getTime())
			? ""
			: `, built ${built.toLocaleDateString(undefined, { dateStyle: "medium" })}`;

		ui.firmwareLatest.textContent = `Latest: version ${latest.info.version}${when}` +
			(running ? ` · On the board: version ${running.version}` : "");
	}
}

function setProgress(fraction, text) {
	ui.progress.value = fraction;
	ui.progressText.textContent = text;
}

async function uploadImage(image) {
	const sha = new Uint8Array(await crypto.subtle.digest("SHA-256", image));
	const started = performance.now();
	let offset = 0;
	let stalled = 0;

	while (offset < image.length) {
		const seq = nextSeq();
		// Packets stay this size even on a small-MTU link, where write()
		// splits them and the board reassembles them.
		const chunk = smp.encodeUploadChunk(image, sha, offset, seq, LARGE_WRITE);
		const payload = await sendPacket(seq, chunk.packet);

		if (typeof payload.off !== "number") {
			throw new Error("Board sent an unexpected upload response");
		}

		// The board says where to continue, which also resumes a partial upload.
		stalled = payload.off === offset ? stalled + 1 : 0;
		if (stalled > 3) {
			throw new Error("Upload is not making progress");
		}
		offset = payload.off;

		const seconds = (performance.now() - started) / 1000;
		const rate = offset / 1024 / Math.max(seconds, 0.1);

		setProgress(offset / image.length,
			    `${Math.round((offset / image.length) * 100)}% · ${rate.toFixed(1)} kB/s`);
	}
}

async function waitForReboot() {
	const deadline = Date.now() + REBOOT_TIMEOUT_MS;

	// Wait for the link to drop, then for the board to come back.
	while (state.device.gatt.connected && Date.now() < deadline) {
		await sleep(250);
	}

	while (Date.now() < deadline) {
		await sleep(3000);
		try {
			await openServices();
			return;
		} catch {
			// Still swapping images; try again.
		}
	}
	throw new Error("The board did not come back after the reset. Reconnect to check its state.");
}

async function upgrade(firmware) {
	state.busy = true;
	updateControls();
	ui.log.replaceChildren();
	ui.upgradeProgress.hidden = false;

	try {
		log(`Uploading version ${firmware.version}…`);
		setProgress(0, "0%");
		await uploadImage(firmware.image);
		log("Upload complete.");

		const uploaded = (await refreshSlots()).find((image) => image.slot === 1);

		if (!uploaded) {
			throw new Error("The uploaded image does not appear in the standby slot");
		}

		await request(smp.OP_WRITE, smp.GROUP_IMAGE, smp.IMAGE_STATE,
			      { hash: uploaded.hash, confirm: false });
		log("Image marked for a test boot. Resetting the board…");

		state.expectDisconnect = true;
		await request(smp.OP_WRITE, smp.GROUP_OS, smp.OS_RESET).catch(() => {
			// The board may reset before its reply gets through.
		});

		setProgress(1, "Swapping images…");
		await waitForReboot();
		state.expectDisconnect = false;
		onConnected();

		const running = (await refreshSlots()).find((image) => image.slot === 0);
		const sameImage = running?.hash && smp.toHex(running.hash) === smp.toHex(uploaded.hash);

		if (sameImage && running.confirmed) {
			log(`Done. The board is running version ${running.version}.`, "ok");
			setProgress(1, "Done");
		} else if (sameImage) {
			log(`Version ${running.version} is running but has not confirmed itself yet. ` +
			    "If it does not, the board reverts on its next reset.", "warn");
			setProgress(1, "Running, not confirmed");
		} else {
			throw new Error("The board is still on its previous firmware: it did not accept " +
					"the new one. Upgrading over USB will get it there.");
		}
	} catch (err) {
		log(err.message, "error");
		setProgress(0, "Failed");
	} finally {
		state.expectDisconnect = false;
		state.busy = false;
		updateControls();
	}
}

// ------------------------------------------------------------------- UI ----

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function log(text, kind = "") {
	const line = document.createElement("li");

	line.textContent = text;
	if (kind) {
		line.className = kind;
	}
	ui.log.append(line);
	line.scrollIntoView({ block: "nearest" });
}

function setStatus(kind, text) {
	ui.status.dataset.state = kind;
	ui.status.textContent = text;
}

function updateControls() {
	const connected = Boolean(state.smpChar);

	ui.connect.textContent = connected ? "Disconnect" : "Connect";
	ui.connect.disabled = state.busy;
	ui.refresh.disabled = !connected || state.busy;
	ui.file.disabled = state.busy;
	ui.upload.disabled = !connected || state.busy || !state.firmware;
	ui.upgradeBle.disabled = !connected || state.busy || !state.latest;
	ui.upgradeBle.title = connected ? "" : "Connect to the board first";
	ui.upgradeUsb.disabled = state.busy || !state.latest;
	if (!BLUETOOTH) {
		ui.connect.disabled = true;
	}
	ui.usbFindLegacy.disabled = state.busy;
	ui.usbFind.disabled = state.busy;
	ui.usbDrive.disabled = state.busy;
	// No shell commands while an upgrade has the link busy.
	controls.setConnected(connected && !state.busy);
}

ui.connect.addEventListener("click", async () => {
	if (state.smpChar) {
		disconnect();
		return;
	}
	try {
		await connect();
	} catch (err) {
		// Closing the device chooser is not an error worth reporting.
		if (err.name !== "NotFoundError") {
			setStatus("disconnected", "Disconnected");
			log(`Could not connect: ${err.message}`, "error");
		}
		updateControls();
	}
});

ui.refresh.addEventListener("click", () => {
	refreshSlots().catch((err) => log(`Could not read image list: ${err.message}`, "error"));
});
ui.file.addEventListener("change", chooseFile);
ui.upload.addEventListener("click", () => upgrade(state.firmware));
ui.upgradeBle.addEventListener("click", () => {
	ui.usbPanel.hidden = true;
	upgrade(state.latest.firmware);
});

// ------------------------------------------------------ Upgrade over USB ---
//
// One press does as much as the browser lets it: restart the board into USB
// mode (where it is a drive, see usb.DRIVE_NAMES), then write the firmware file onto
// that drive. The browser has the user pick a USB device and the drive the
// first time; after that the page may use them without asking.

const DRIVE_WAIT_MS = 12000; // a restart into USB mode takes a few seconds
const DOUBLE_TAP_WAIT_MS = 60000;
const DOUBLE_TAP = "Double-tap the board's reset button";

/** The remembered drives the page may write to. */
async function usableDrives(ask = false) {
	const drives = [];

	for (const drive of await usb.rememberedDrives()) {
		if (await usb.mayWriteDrive(drive, ask)) {
			drives.push(drive);
		}
	}
	return drives;
}

function showUsbResult(text, kind = "") {
	ui.usbResult.textContent = text;
	ui.usbResult.className = `note ${kind}`;
}

async function downloadUsbFirmware() {
	const name = state.latest.info.usb;
	const bytes = new Uint8Array(await (await fetchFirmwareFile(name)).arrayBuffer());

	if (!usb.isUf2(bytes)) {
		throw new Error(`${name} is not a firmware file`);
	}
	return bytes;
}

let usbAttempt = 0; // counts presses, so that an overtaken wait can tell

function usbBusy(busy) {
	if (busy) {
		usbAttempt++;
	}
	state.busy = busy;
	updateControls();
}

/** Starts an upgrade: gets the board into USB mode by whatever way needs no asking. */
async function upgradeOverUsb() {
	ui.usbPanel.hidden = false;
	ui.usbChoices.hidden = true;
	ui.usbDrive.hidden = true;
	ui.log.replaceChildren();
	usbBusy(true);
	try {
		// First, while the press still counts for the browser's permission.
		const drives = await usableDrives(true);
		const mounted = await usb.mountedDrive(drives);

		if (mounted) {
			await installFromDrive(mounted);
			return;
		}

		let kind = null;

		if (state.smpChar) {
			showUsbResult("Restarting the board into USB mode…");
			await shell.fire("bootloader");
			kind = usb.KIND_CURRENT;
		} else {
			showUsbResult("Looking for the board on USB…");
			kind = await usb.restartKnownIntoUsbMode();
		}

		if (kind) {
			await afterRestart(kind, drives);
		} else {
			ui.usbFindLegacy.hidden = !usb.HID_SUPPORTED;
			ui.usbFind.hidden = !usb.SERIAL_SUPPORTED;
			ui.usbChoices.hidden = !usb.HID_SUPPORTED && !usb.SERIAL_SUPPORTED;
			ui.usbDrive.hidden = !usb.DRIVE_SUPPORTED;
			showUsbResult(ui.usbChoices.hidden
				? `${DOUBLE_TAP} to put it in USB mode.`
				: "Plug the board into this computer and choose what it is. If its " +
				  `drive (${usb.DRIVE_NAMES}) is already showing, choose the drive instead.`);
		}
	} catch (err) {
		showUsbResult(`Could not upgrade over USB (${err.message}). ${DOUBLE_TAP} to put ` +
			      "the board in USB mode, then choose its drive.", "error");
		ui.usbDrive.hidden = !usb.DRIVE_SUPPORTED;
	} finally {
		usbBusy(false);
	}
}

/**
 * The board has been told to restart into USB mode: carry on to its drive.
 * A board that does not get there has to be double-tapped.
 */
async function afterRestart(kind, drives) {
	const found = kind === usb.KIND_BOOTLOADER
		? `Found ${usb.describe(kind)}.`
		: `Found ${usb.describe(kind)} and told it to restart into USB mode.`;

	ui.usbChoices.hidden = true;

	if (!usb.DRIVE_SUPPORTED) {
		ui.usbManual.open = true;
		showUsbResult(`${found} A drive named ${usb.DRIVE_NAMES} appears: download the ` +
			      "firmware file and copy it there. If no drive appears, " +
			      `${DOUBLE_TAP.toLowerCase()}.`, "ok");
		return;
	}

	drives = drives || await usableDrives();
	if (kind === usb.KIND_UNKNOWN) {
		showUsbResult(`Found ${usb.describe(kind)}. ${DOUBLE_TAP} to put it in USB ` +
			      "mode.", "error");
	} else if (!drives.length) {
		ui.usbDrive.hidden = false;
		showUsbResult(`${found} When the drive named ${usb.DRIVE_NAMES} appears, choose ` +
			      "it to install the firmware. If none appears within a few seconds, " +
			      `${DOUBLE_TAP.toLowerCase()}.`, "ok");
		return;
	} else {
		showUsbResult(`${found} Waiting for its drive…`);
	}

	let drive = kind === usb.KIND_UNKNOWN
		? null
		: await usb.waitForDrive(drives, DRIVE_WAIT_MS);

	if (!drive) {
		// It did not switch by itself. Its drive may be one not seen before
		// (the two names), so the button stays while known drives are watched.
		ui.usbDrive.hidden = false;
		showUsbResult(`The board did not switch to USB mode. ${DOUBLE_TAP}` +
			      (drives.length ? "." : ", then choose its drive " +
			      `(${usb.DRIVE_NAMES}).`), "error");
		if (!drives.length) {
			return;
		}
		// Not busy meanwhile, so that the drive can be chosen by hand.
		usbBusy(false);

		const attempt = usbAttempt;

		drive = await usb.waitForDrive(drives, DOUBLE_TAP_WAIT_MS);
		if (!drive || attempt !== usbAttempt) {
			return;
		}
		usbBusy(true);
	}
	await installFromDrive(drive);
}

/** Writes the latest firmware onto the board's drive. */
async function installFromDrive(drive) {
	ui.usbChoices.hidden = true;
	ui.usbDrive.hidden = true;
	showUsbResult("Downloading the firmware…");

	const bytes = await downloadUsbFirmware();

	showUsbResult("Installing. Leave the board plugged in.");
	ui.upgradeProgress.hidden = false;
	try {
		const taken = await usb.installOnDrive(drive, state.latest.info.usb, bytes,
			(fraction) => setProgress(fraction, `${Math.round(fraction * 100)} %`));

		if (taken) {
			showUsbResult(`Firmware ${state.latest.info.version} is installed. The board ` +
				      "has restarted with it.", "ok");
		} else {
			ui.usbDrive.hidden = false;
			showUsbResult("The board did not take the firmware. Choose the drive to " +
				      "try again.", "error");
		}
	} finally {
		ui.upgradeProgress.hidden = true;
	}
}

// Runs one step that the user started with a button.
async function usbStep(waiting, step) {
	usbBusy(true);
	showUsbResult(waiting);
	try {
		await step();
	} catch (err) {
		showUsbResult({
			AbortError: "Nothing was chosen.",
			NotFoundError: "That is not the board. For the drive, choose the one named " +
				       `${usb.DRIVE_NAMES}; if the board is not listed, ` +
				       `${DOUBLE_TAP.toLowerCase()}.`,
		}[err.name] || `Could not reach the board (${err.message}). ${DOUBLE_TAP} to put ` +
			       "it in USB mode, then choose its drive.", "error");
		ui.usbDrive.hidden = !usb.DRIVE_SUPPORTED;
	} finally {
		usbBusy(false);
		checkLegacyOnUsb();
	}
}

ui.upgradeUsb.addEventListener("click", upgradeOverUsb);
ui.usbFindLegacy.addEventListener("click", () => usbStep(
	"Choose the board in the list the browser shows…", async () => {
		if (!(await usb.restartLegacyIntoUsbMode())) {
			throw new DOMException("Nothing chosen", "AbortError");
		}
		await afterRestart(usb.KIND_LEGACY);
	}));
ui.usbFind.addEventListener("click", () => usbStep(
	"Choose the board in the list the browser shows…",
	async () => afterRestart(await usb.restartSerialIntoUsbMode())));
ui.usbDrive.addEventListener("click", () => usbStep(
	`Choose the drive named ${usb.DRIVE_NAMES}…`,
	async () => installFromDrive(await usb.chooseDrive())));

// A legacy Cato the page has been given access to before is noticed as soon
// as it is plugged in (the browser reveals no others without being asked).
async function checkLegacyOnUsb() {
	const present = (await usb.knownLegacyDevices().catch(() => [])).length > 0;

	if (present !== state.legacyOnUsb) {
		state.legacyOnUsb = present;
		showFirmwareStatus();
	}
}

if (usb.HID_SUPPORTED) {
	navigator.hid.addEventListener("connect", checkLegacyOnUsb);
	navigator.hid.addEventListener("disconnect", checkLegacyOnUsb);
	checkLegacyOnUsb();
}

ui.clear.addEventListener("click", () => {
	layout.clear();
	term.focus();
});

window.addEventListener("beforeunload", (event) => {
	if (state.busy) {
		event.preventDefault();
	}
});

// Installable and usable offline (see sw.js).
if ("serviceWorker" in navigator) {
	navigator.serviceWorker.register("sw.js").catch((err) => {
		console.warn("Service worker registration failed:", err);
	});
}

if (!BLUETOOTH) {
	ui.unsupported.hidden = false;
	ui.connect.disabled = true;
	// Bluetooth upgrades are not offered: USB is the way.
	document.body.classList.add("no-bluetooth");
	ui.upgradeUsb.classList.add("primary");
	// Nor, without Bluetooth, is there usually any way to reach USB devices.
	if (!usb.DRIVE_SUPPORTED) {
		ui.usbPanel.hidden = false;
		ui.usbManual.open = true;
	}
} else {
	updateControls();
}
setStatus("disconnected", "Disconnected");
loadLatest();
