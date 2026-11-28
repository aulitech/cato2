// SPDX-License-Identifier: MIT
//
// Control cards: a GUI for the board's shell commands (name, blink, imu,
// bonds, sleep, bootloader). Each control runs the same command a user would
// type in the console and shows the board's own reply or error.

import { ask } from "./dialog.js";
import { parseBlink, parseBonds, parseHid, parseImu, parseImuRead, ShellError } from "./shell.js";

const $ = (id) => document.getElementById(id);

const BLINK_STEPS = {
	"r": "#e5484d",
	"g": "#30a46c",
	"b": "#3e82f7",
	"y": "#f5d90a",
	"c": "#22c3d6",
	"m": "#d24bd0",
	"w": "#ffffff",
	"-": "transparent",
};

const ODR_OPTIONS = ["0", "12.5", "26", "52", "104", "208", "416", "833", "1660", "3330", "6660"];
const RANGE_OPTIONS = {
	accel: [["2", "±2 g"], ["4", "±4 g"], ["8", "±8 g"], ["16", "±16 g"]],
	gyro: [["125", "±125 dps"], ["250", "±250 dps"], ["500", "±500 dps"],
	       ["1000", "±1000 dps"], ["2000", "±2000 dps"]],
};
const MODE_OPTIONS = [["high", "High perf."], ["low", "Low power"]];

const HID_RATE_OPTIONS = [["10", "10 Hz"], ["25", "25 Hz"], ["50", "50 Hz"], ["100", "100 Hz"]];
const HID_REPORTS = [["mouse", "Mouse"], ["keyboard", "Keyboard"], ["accel", "Accelerometer"],
		     ["gyro", "Gyroscope"], ["orientation", "Orientation"]];

const LIVE_INTERVAL_MS = 400;

/**
 * @param shell   ShellRunner
 * @param onName  (string) => void, called when the board's name is known
 */
const BOND_NAME_MAX = 14; // as on the board

export function initControls({ shell, onName, onForward, onFactoryReset, onLive }) {
	const cards = document.querySelectorAll("#controls .card:not([data-card='firmware'])");
	let connected = false;
	let liveTimer = null;

	// ------------------------------------------------------------ helpers ---

	function message(card, text = "", isError = false) {
		const el = document.querySelector(`[data-card="${card}"] .message`);

		el.textContent = text;
		el.classList.toggle("error", isError);
	}

	// Controls inside .developer-only stay disabled without Developer mode
	// (Settings): what they show can be read, not changed.
	const developerMode = () => document.body.classList.contains("developer");

	function setEnabled(enabled) {
		for (const card of cards) {
			for (const el of card.querySelectorAll("button, input, select")) {
				el.disabled = !enabled || (!developerMode() && Boolean(el.closest(".developer-only")));
			}
		}
	}

	window.addEventListener("developer-mode", () => setEnabled(connected));

	/** Runs one card action, reporting the board's error message if it fails. */
	async function act(card, action, done = "") {
		message(card);
		try {
			await action();
			message(card, done);
			return true;
		} catch (err) {
			if (connected) {
				message(card, err.message, true);
			}
			return false;
		}
	}

	function fill(select, options) {
		for (const option of options) {
			const [value, label] = Array.isArray(option) ? option : [option, option];

			select.add(new Option(label, value));
		}
	}

	/** Quotes free text as one shell argument. */
	function quote(text) {
		if (/["\\]/.test(text)) {
			throw new Error('Quotes (") and backslashes (\\) are not supported');
		}
		return `"${text}"`;
	}

	// -------------------------------------------------------------- board ---

	async function loadName() {
		const [name] = await shell.run("name");

		$("name-input").value = name || "";
		onName(name || "");
	}

	$("name-apply").addEventListener("click", () => act("device", async () => {
		const name = $("name-input").value.trim();

		if (!name) {
			throw new Error("Enter a name");
		}
		await shell.run(`name ${quote(name)}`);
		await loadName();
	}, "Renamed. Other devices may show the old name for a while."));

	$("reboot").addEventListener("click", async () => {
		if (await ask("Restart the board?\n\nYou are disconnected for a moment; settings are kept.",
			      { action: "Restart" })) {
			act("device", () => shell.fire("reboot"));
		}
	});

	$("factory-reset").addEventListener("click", async () => {
		if (await ask("Reset the board to factory defaults?\n\nThis erases its name, blink, " +
			      "IMU and HID settings and all paired devices, then restarts it. The app " +
			      "forgets the board and does not reconnect.\n\nTo connect again, first " +
			      "remove (forget) the board in the Bluetooth settings of this and every " +
			      "other paired device.", { action: "Reset", danger: true })) {
			act("device", async () => {
				await shell.fire("factory_reset confirm");
				await onFactoryReset();
			});
		}
	});

	$("bootloader").addEventListener("click", async () => {
		if (await ask("Restart into the USB bootloader?\n\nThe board disconnects and stays " +
			      "in bootloader mode until firmware is copied to its USB drive or it is " +
			      "reset.", { action: "Restart" })) {
			act("device", () => shell.fire("bootloader"));
		}
	});

	// -------------------------------------------------------------- blink ---

	function drawBlinkPreview() {
		const preview = $("blink-preview");

		preview.replaceChildren();
		for (const step of $("blink-pattern").value) {
			const dot = document.createElement("span");

			dot.style.setProperty("--swatch", BLINK_STEPS[step] ?? "transparent");
			dot.title = step in BLINK_STEPS ? step : `${step} (not a valid step)`;
			preview.append(dot);
		}
	}

	for (const [step, colour] of Object.entries(BLINK_STEPS)) {
		const button = document.createElement("button");
		const swatch = document.createElement("span");

		swatch.className = "swatch";
		swatch.style.setProperty("--swatch", colour);
		button.append(swatch);
		button.title = step === "-" ? "Off" : `Add ${step}`;
		button.setAttribute("aria-label", button.title);
		button.addEventListener("click", () => {
			const input = $("blink-pattern");

			if (input.value.length < input.maxLength) {
				input.value += step;
				drawBlinkPreview();
			}
		});
		$("blink-palette").append(button);
	}

	$("blink-backspace").addEventListener("click", () => {
		$("blink-pattern").value = $("blink-pattern").value.slice(0, -1);
		drawBlinkPreview();
	});
	$("blink-pattern").addEventListener("input", drawBlinkPreview);

	async function loadBlink() {
		const blink = parseBlink(await shell.run("blink"));

		$("blink-rate").value = blink.rate;
		$("blink-pattern").value = blink.pattern;
		drawBlinkPreview();
	}

	$("blink-apply").addEventListener("click", () => act("blink", async () => {
		try {
			await shell.run(`blink rate ${$("blink-rate").value || 0}`);
			await shell.run(`blink pattern ${quote($("blink-pattern").value)}`);
		} finally {
			await loadBlink().catch(() => {});
		}
	}, "Applied and saved."));

	// ---------------------------------------------------------------- imu ---

	for (const sensor of ["accel", "gyro"]) {
		fill($(`${sensor}-odr`), ODR_OPTIONS.map((odr) => [odr, odr === "0" ? "Off" : odr]));
		fill($(`${sensor}-range`), RANGE_OPTIONS[sensor]);
		fill($(`${sensor}-mode`), MODE_OPTIONS);

		for (const setting of ["odr", "range", "mode"]) {
			const select = $(`${sensor}-${setting}`);

			select.addEventListener("change", () => act("imu", async () => {
				try {
					await shell.run(`imu ${sensor} ${setting} ${select.value}`);
				} finally {
					await loadImu().catch(() => {});
				}
			}, "Applied. Save to keep it after a reset."));
		}
	}

	async function loadImu() {
		const imu = parseImu(await shell.run("imu"));

		for (const sensor of ["accel", "gyro"]) {
			$(`${sensor}-odr`).value = imu[sensor].odr;
			$(`${sensor}-range`).value = imu[sensor].range;
			$(`${sensor}-mode`).value = imu[sensor].mode;
		}
		$("imu-saved").textContent = imu.saved ? "Saved configuration" : "Default configuration";
	}

	async function readImu(quiet) {
		const reading = parseImuRead(await shell.run("imu read", { quiet }));

		["x", "y", "z"].forEach((axis, i) => {
			$(`accel-${axis}`).textContent = reading.accel[i].toFixed(2);
			$(`gyro-${axis}`).textContent = reading.gyro[i].toFixed(2);
		});
		$("imu-temp").textContent = reading.temp === null ? "–" : reading.temp.toFixed(1);
	}

	// Readings are live while the card is open: read again and again, kept out
	// of the console so that it stays readable.
	const imuCard = document.querySelector('[data-card="imu"]');
	let liveRunning = false;
	let livePaused = false; // by the button on the card
	let liveReported = false;

	function stopLive() {
		clearTimeout(liveTimer);
		liveTimer = null;
	}

	const liveWanted = () => connected && !livePaused && imuCard.classList.contains("open");

	async function liveTick() {
		liveTimer = null;
		if (liveRunning || !liveWanted()) {
			return;
		}
		liveRunning = true;
		try {
			await readImu(true);
		} catch {
			// A reading lost, to a busy link or a board on its way out: the
			// next one is tried all the same, while the card stays open.
		}
		liveRunning = false;
		if (liveWanted() && liveTimer === null) {
			liveTimer = setTimeout(liveTick, LIVE_INTERVAL_MS);
		}
	}

	function syncLive() {
		const live = liveWanted();
		const toggle = $("imu-live-toggle");

		toggle.textContent = livePaused ? "Play" : "Pause";
		toggle.title = livePaused ? "Read the sensor continuously" : "Stop reading the sensor";
		toggle.setAttribute("aria-pressed", String(!livePaused));
		$("imu-live-note").textContent = livePaused ? "Readings paused" : "";

		// The readings share the console's line to the board, and each one
		// would wipe what is being typed there: the console is told to hold off.
		if (live !== liveReported) {
			liveReported = live;
			onLive(live);
		}

		if (!liveWanted()) {
			stopLive();
		} else if (liveTimer === null && !liveRunning) {
			liveTick();
		}
	}

	$("imu-live-toggle").addEventListener("click", () => {
		livePaused = !livePaused;
		syncLive();
	});

	new MutationObserver(syncLive).observe(imuCard, { attributes: true, attributeFilter: ["class"] });

	$("imu-save").addEventListener("click", () => act("imu", async () => {
		await shell.run("imu save");
		await loadImu();
	}, "Saved; applied again at every boot."));

	$("imu-reset").addEventListener("click", async () => {
		if (await ask("Return the IMU to its default configuration?\n\nThe saved " +
			      "configuration is forgotten.", { action: "Reset", danger: true })) {
			act("imu", async () => {
				await shell.run("imu reset");
				await loadImu();
			}, "Reset to defaults.");
		}
	});

	$("reg-read").addEventListener("click", () => act("imu", async () => {
		const lines = await shell.run(
			`imu reg read ${$("reg-read-addr").value.trim()} ${$("reg-read-count").value || 1}`,
			{ timeout: 8000 });

		$("imu-reg-output").textContent = lines.join("\n");
	}));

	$("reg-write").addEventListener("click", () => act("imu", async () => {
		const addr = $("reg-write-addr").value.trim();

		await shell.run(`imu reg write ${addr} ${$("reg-write-value").value.trim()}`);
		$("imu-reg-output").textContent = (await shell.run(`imu reg read ${addr}`)).join("\n");
		await loadImu();
	}, "Written."));

	// -------------------------------------------------------------- bonds ---

	async function removeBond(bond) {
		const warning = bond.connected
			? "\n\nThis is the device you are connected with: removing it disconnects you, " +
			  "and you must also forget the board in this device's Bluetooth settings " +
			  "before pairing again."
			: "";

		if (!(await ask(`Remove ${bond.name || bond.address}?${warning}`,
				{ action: "Remove", danger: true }))) {
			return;
		}
		act("bonds", async () => {
			if (bond.connected) {
				await shell.fire(`bonds remove ${bond.index}`);
			} else {
				await shell.run(`bonds remove ${bond.index}`);
				await loadBonds();
			}
		});
	}

	async function loadBonds() {
		const state = parseBonds(await shell.run("bonds"));

		// Bonds are numbered as listed: a changed list means changed numbers.
		if (state.bonds.map((bond) => bond.address).join() !==
		    knownBonds.map((bond) => bond.address).join()) {
			hidPeer = null;
		}
		knownBonds = state.bonds;
		const list = $("bonds-list");

		list.replaceChildren();
		for (const bond of state.bonds) {
			const row = list.insertRow();
			const remove = document.createElement("button");

			const name = document.createElement("input");
			const address = document.createElement("div");

			row.insertCell().textContent = bond.index;

			// A name for the device, kept on the board; its address underneath.
			name.type = "text";
			name.maxLength = BOND_NAME_MAX;
			name.value = bond.name;
			name.placeholder = "Name this device";
			name.setAttribute("aria-label", `Name of paired device ${bond.index}`);
			name.addEventListener("change", () => act("bonds", async () => {
				try {
					await shell.run(`bonds name ${bond.index} ${name.value.trim()}`.trim());
				} finally {
					await loadBonds().catch(() => {});
				}
			}, "Saved."));
			address.className = "note mono";
			address.textContent = bond.address;
			row.insertCell().append(name, address);
			row.insertCell().textContent = bond.connected ? "this device" : "";
			remove.textContent = "Remove";
			remove.className = "small";
			remove.addEventListener("click", () => removeBond(bond));
			row.insertCell().append(remove);
		}
		if (!state.bonds.length) {
			list.insertRow().insertCell().textContent = "No paired devices.";
		}

		$("bonds-summary").textContent = `${state.used} of ${state.max} used`;
		$("bonds-pairing").checked = state.pairing;
	}

	$("bonds-pairing").addEventListener("change", () => act("bonds", async () => {
		try {
			await shell.run(`bonds pairing ${$("bonds-pairing").checked ? "on" : "off"}`);
		} finally {
			await loadBonds().catch(() => {});
		}
	}));

	$("bonds-remove-all").addEventListener("click", async () => {
		if (await ask("Remove all paired devices, including this one?\n\nYou are " +
			      "disconnected, and each device must forget the board in its Bluetooth " +
			      "settings before pairing again.", { action: "Remove all", danger: true })) {
			act("bonds", () => shell.fire("bonds remove all"));
		}
	});

	// -------------------------------------------------------------- sleep ---

	$("sleep-motion").addEventListener("click", async () => {
		const mg = $("sleep-threshold").value || 125;

		if (await ask(`Put the board to sleep?\n\nIt wakes when it is moved (more than ${mg} mg) ` +
			      "or reset.", { action: "Sleep" })) {
			act("sleep", () => shell.fire(`sleep motion ${mg}`));
		}
	});

	$("sleep-off").addEventListener("click", async () => {
		if (await ask("Put the board into deep sleep?\n\nOnly its reset button wakes it.",
			      { action: "Sleep" })) {
			act("sleep", () => shell.fire("sleep off"));
		}
	});

	// ---------------------------------------------------------------- hid ---

	fill($("hid-rate"), HID_RATE_OPTIONS);

	$("hid-peer").addEventListener("change", () => act("hid", async () => {
		hidPeer = Number($("hid-peer").value);
		try {
			await shell.run(`hid peer ${hidPeer}`);
		} finally {
			await loadHid().catch(() => {});
		}
	}));

	const hidSwitches = {};
	let knownBonds = []; // as last listed, for the choice of device
	let hidPeer = null; // the number of the device the settings shown are for
	let airSupported = false; // firmware from before the air mouse has no such command

	for (const [report, label] of HID_REPORTS) {
		const wrapper = document.createElement("label");
		const box = document.createElement("input");

		box.type = "checkbox";
		box.disabled = true;
		box.addEventListener("change", () => act("hid", async () => {
			try {
				await shell.run(`hid ${box.checked ? "enable" : "disable"} ${report}`);
				// The air mouse has no switch of its own: it goes with the mouse.
				if (report === "mouse" && airSupported) {
					await shell.run(`hid air ${box.checked ? "on" : "off"}`);
				}
			} finally {
				await loadHid().catch(() => {});
			}
		}, "Applied and saved."));
		wrapper.append(box, ` ${label}`);
		$("hid-reports").append(wrapper);
		hidSwitches[report] = box;
	}

	const AIR_AXES = ["+x", "-x", "+y", "-y", "+z", "-z"]
		.map((axis) => [axis, `gyro ${axis[1].toUpperCase()}${axis[0] === "-" ? ", reversed" : ""}`]);

	fill($("air-axis-x"), AIR_AXES);
	fill($("air-axis-y"), AIR_AXES);
	const airSetting = (id, command) => $(id).addEventListener("change", () => act("hid", async () => {
		try {
			await shell.run(command());
		} finally {
			await loadHid().catch(() => {});
		}
	}, "Applied and saved."));

	const airSpeed = () => `hid air speed ${$("air-speed-x").value} ${$("air-speed-y").value}`;

	airSetting("air-speed-x", airSpeed);
	airSetting("air-speed-y", airSpeed);
	airSetting("air-accel", () => `hid air accel ${$("air-accel").value}`);
	airSetting("air-deadzone", () => `hid air deadzone ${$("air-deadzone").value}`);
	// Until fixed axes have been chosen once, start from the firmware's defaults.
	const manualAxes = () =>
		`hid air axes ${$("air-axis-x").value || "-z"} ${$("air-axis-y").value || "-x"}`;

	airSetting("air-mode", () => ($("air-mode").value === "auto" ? "hid air axes auto" : manualAxes()));
	airSetting("air-axis-x", manualAxes);
	airSetting("air-axis-y", manualAxes);

	// Mouse, keyboard and sensor settings are kept for each paired device. The
	// board is told which one the commands are about ("hid peer"); to begin
	// with that is this device, the one the app is connected from.
	async function readHid() {
		try {
			return parseHid(await shell.run("hid"));
		} catch (err) {
			// With several devices paired the board wants one chosen first.
			if (!(err instanceof ShellError) || !/hid peer/.test(err.message)) {
				throw err;
			}
			await shell.run(`hid peer ${(knownBonds.find((bond) => bond.connected) ||
						     knownBonds[0] || { index: 1 }).index}`);
			return parseHid(await shell.run("hid"));
		}
	}

	async function loadHid() {
		let hid = await readHid();

		if (hid.peer) {
			const mine = knownBonds.find((bond) => bond.connected);
			const wanted = hidPeer || (mine && mine.index);

			// Someone else may have chosen another device since.
			if (wanted && wanted !== hid.peer.index &&
			    knownBonds.some((bond) => bond.index === wanted)) {
				await shell.run(`hid peer ${wanted}`);
				hid = parseHid(await shell.run("hid"));
			}
			hidPeer = hid.peer.index;
		}

		const select = $("hid-peer");

		$("hid-peer-row").hidden = !hid.peer;
		select.replaceChildren(...(hid.peer ? knownBonds : []).map((bond) => new Option(
			`${bond.name || bond.address}${bond.connected ? " (this device)" : ""}`,
			bond.index)));
		if (hid.peer) {
			select.value = String(hid.peer.index);
		}

		const rate = String(hid.rate);

		airSupported = Boolean(hid.air);
		// Shown, and set, on the orientation viewer.
		onForward(hid.air ? hid.air.forward : null);
		$("hid-air").hidden = !hid.air;
		if (hid.air) {
			$("air-state").textContent = hid.air.on && hid.enabled.includes("mouse") ? "(on)" : "(off)";
			$("air-speed-x").value = hid.air.speedX;
			$("air-speed-y").value = hid.air.speedY;
			$("air-accel").value = hid.air.accel;
			$("air-deadzone").value = hid.air.deadzone;
			$("air-mode").value = hid.air.auto ? "auto" : "manual";
			$("air-manual").hidden = hid.air.auto;
			if (!hid.air.auto) {
				$("air-axis-x").value = hid.air.axisX;
				$("air-axis-y").value = hid.air.axisY;
			}
		}

		for (const [report, box] of Object.entries(hidSwitches)) {
			box.checked = hid.enabled.includes(report);
		}
		if (![...$("hid-rate").options].some((option) => option.value === rate)) {
			$("hid-rate").add(new Option(`${rate} Hz`, rate));
		}
		$("hid-rate").value = rate;
		$("hid-hosts").textContent = hid.listening.length
			? `${hid.hosts} connected, in use`
			: "No computer is using it yet";
	}

	// When this computer is one of the board's HID hosts, what the board types
	// and clicks comes straight back to this page: a typed space or Enter
	// presses whichever button has focus, and a mouse click lands on the button
	// under the pointer. Left alone, that button would run its command again,
	// and again. So focus is taken off the button first, and clicks are ignored
	// while a command runs and for a moment after it.
	const HID_ECHO_MS = 500;
	let hidBusy = false;
	let hidQuietUntil = 0;

	const hidAction = (id, command) => $(id).addEventListener("click", async () => {
		if (hidBusy || performance.now() < hidQuietUntil) {
			return;
		}
		hidBusy = true;
		document.activeElement?.blur();
		try {
			await act("hid", () => shell.run(
				typeof command === "function" ? command() : command, { timeout: 10000 }));
		} finally {
			hidBusy = false;
			hidQuietUntil = performance.now() + HID_ECHO_MS;
		}
	});

	hidAction("hid-move", () => `hid mouse move ${$("hid-dx").value || 0} ${$("hid-dy").value || 0}`);
	hidAction("hid-click-left", "hid mouse click left");
	hidAction("hid-click-right", "hid mouse click right");
	hidAction("hid-scroll-up", "hid mouse scroll 3");
	hidAction("hid-scroll-down", "hid mouse scroll -3");
	hidAction("hid-key-send", () => {
		const key = $("hid-key").value.trim();

		if (!key || /\s/.test(key)) {
			throw new Error("Enter one key, such as enter or ctrl+c");
		}
		quote(key);
		return `hid key ${key}`;
	});
	hidAction("hid-type", () => {
		if (!$("hid-text").value) {
			throw new Error("Enter some text");
		}
		// Typing takes about 30 ms a character.
		return `hid type ${quote($("hid-text").value)}`;
	});

	$("hid-rate").addEventListener("change", () => act("hid", async () => {
		try {
			await shell.run(`hid rate ${$("hid-rate").value}`);
		} finally {
			await loadHid().catch(() => {});
		}
	}, "Applied and saved."));

	// ---------------------------------------------------------- lifecycle ---

	async function refresh() {
		await act("device", loadName);
		await act("blink", loadBlink);
		await act("imu", loadImu);
		await act("bonds", loadBonds);
		await act("hid", loadHid);
	}

	$("refresh-all").addEventListener("click", refresh);

	return {
		refresh,
		/** Sets the board axis that points at the screen, for the pointer. */
		async setForward(axis) {
			try {
				await shell.run(`hid air forward ${axis}`);
			} finally {
				await loadHid().catch(() => {});
			}
		},
		setConnected(value) {
			connected = value;
			setEnabled(value);
			syncLive();
		},
	};
}
