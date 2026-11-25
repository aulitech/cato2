// SPDX-License-Identifier: MIT
//
// BLE console (Nordic UART Service) and OTA upgrade (MCUmgr SMP) over one
// Web Bluetooth connection.

import { initControls } from "./controls.js";
import { initOrientation, ORIENTATION_SERVICE, ORIENTATION_VALUE } from "./orientation.js";
import { ShellRunner } from "./shell.js";
import * as smp from "./smp.js";

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
	latest: $("latest"),
	latestInfo: $("latest-info"),
	latestUse: $("latest-use"),
	latestUf2: $("latest-uf2"),
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
	firmware: null,
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

term.onData((data) => {
	if (state.nusRx) {
		write(state.nusRx, encoder.encode(data)).catch(() => {});
	}
});

// The control cards run shell commands over the same stream as the console.
const shell = new ShellRunner({
	send: (text) => {
		if (!state.nusRx) {
			return Promise.reject(new Error("Not connected"));
		}
		return write(state.nusRx, encoder.encode(text));
	},
	display: (bytes) => term.write(bytes),
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
	term.write(`\r\n\x1b[2m-- ${text} --\x1b[0m\r\n`);
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

	renderSlots(images);
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

async function showLatestFirmware() {
	let latest;

	try {
		latest = await (await fetchFirmwareFile("firmware.json")).json();
	} catch {
		return; // none published: only the file picker is offered
	}
	if (!latest.dfu || !latest.usb) {
		return;
	}

	const built = new Date(latest.built);
	const when = Number.isNaN(built.getTime()) ? "" : ` · built ${built.toLocaleString()}`;

	ui.latestInfo.textContent =
		`Latest build: version ${latest.version} · ${String(latest.commit).slice(0, 7)}${when}`;
	ui.latestUf2.href = RELEASE_DIR + latest.usb;
	ui.latestUse.addEventListener("click", async () => {
		ui.file.value = "";
		ui.latestUse.disabled = true;
		await selectFirmware(latest.dfu, async () =>
			new Uint8Array(await (await fetchFirmwareFile(latest.dfu)).arrayBuffer()),
			"Latest build: ");
	});
	ui.latest.hidden = false;
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

async function upgrade() {
	const firmware = state.firmware;

	state.busy = true;
	updateControls();
	ui.log.replaceChildren();

	try {
		log(`Uploading ${firmware.name} (version ${firmware.version})…`);
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
			throw new Error("The board came back on its previous image: the new one was rejected or reverted.");
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
	ui.latestUse.disabled = state.busy;
	ui.upload.disabled = !connected || state.busy || !state.firmware;
	// No shell commands while an upgrade has the link busy.
	controls.setConnected(connected && !state.busy);
}

// Side-panel cards fold away: all closed to begin with, one open at a time,
// and the one just opened is brought to the top of the panel.
function initAccordion() {
	const cards = [...document.querySelectorAll("#controls .card")];

	for (const card of cards) {
		const head = card.querySelector(".card-head");

		const toggle = () => {
			const open = !card.classList.contains("open");

			for (const other of cards) {
				other.classList.toggle("open", open && other === card);
				other.querySelector(".card-head")
					.setAttribute("aria-expanded", String(open && other === card));
			}
			if (open) {
				card.scrollIntoView({ block: "start", behavior: "smooth" });
			}
		};

		head.setAttribute("role", "button");
		head.setAttribute("tabindex", "0");
		head.setAttribute("aria-expanded", "false");
		head.addEventListener("click", (event) => {
			// Buttons in the heading (Refresh) act; they do not fold the card.
			if (!event.target.closest("button")) {
				toggle();
			}
		});
		head.addEventListener("keydown", (event) => {
			if (event.target === head && (event.key === "Enter" || event.key === " ")) {
				event.preventDefault();
				toggle();
			}
		});
	}
}

initAccordion();

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
ui.upload.addEventListener("click", upgrade);
ui.clear.addEventListener("click", () => {
	term.clear();
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

if (!navigator.bluetooth) {
	ui.unsupported.hidden = false;
	ui.connect.disabled = true;
} else {
	updateControls();
}
setStatus("disconnected", "Disconnected");
showLatestFirmware();
