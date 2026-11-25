// SPDX-License-Identifier: MIT
//
// Runs shell commands for the GUI over the same byte stream as the console.
//
// A command is typed for the user (after Ctrl-U, to clear anything already on
// the line), and everything the board sends until its next prompt is collected
// as the reply. No browser APIs, so this module also runs under Node.

const ANSI = /\x1b\[[0-9;]*[A-Za-z]/g;
const ERROR_COLOUR = "\x1b[1;31m";
const PROMPT = /[\w.-]+:~\$ $/;
const LOG_LINE = /^\[\d\d:\d\d:\d\d\.\d{3},\d{3}\] <\w+> /;
const CLEAR_LINE = "\x15";

export class ShellError extends Error {}

export class ShellRunner {
	/**
	 * @param send     async (string) => void, writes to the board
	 * @param display  (Uint8Array) => void, shows console output to the user
	 */
	constructor({ send, display }) {
		this.send = send;
		this.display = display;
		this.decoder = new TextDecoder();
		this.active = null;
		this.queue = Promise.resolve();
	}

	/** Every chunk received from the board goes through here. */
	feed(bytes) {
		const active = this.active;

		if (!active || !active.quiet) {
			this.display(bytes);
		}
		if (!active) {
			return;
		}

		active.text += this.decoder.decode(bytes, { stream: true });

		// The echoed command ends the first line; the reply follows it and is
		// complete once the prompt is back.
		const echoEnd = active.text.indexOf("\n");

		if (echoEnd < 0) {
			return;
		}

		const raw = active.text.slice(echoEnd + 1);
		const reply = raw.replace(ANSI, "");

		if (!PROMPT.test(reply)) {
			return;
		}

		const lines = reply
			.split(/\r?\n/)
			.slice(0, -1)
			.map((line) => line.trimEnd())
			.filter((line) => line !== "" && !LOG_LINE.test(line));

		this.finish(active, raw.includes(ERROR_COLOUR)
			? new ShellError(lines.join(" ") || "Command failed")
			: null, lines);
	}

	finish(active, error, lines) {
		if (this.active !== active) {
			return;
		}
		clearTimeout(active.timer);
		this.active = null;
		if (error) {
			active.reject(error);
		} else {
			active.resolve(lines);
		}
	}

	/**
	 * Runs one command and resolves with its output lines. Rejects with a
	 * ShellError carrying the board's message if the command reports an error.
	 * With quiet, the exchange is kept out of the console.
	 */
	run(command, { quiet = false, timeout = 4000 } = {}) {
		const result = this.queue.then(() => new Promise((resolve, reject) => {
			const active = { text: "", quiet, resolve, reject, timer: null };

			active.timer = setTimeout(() => {
				this.finish(active, new Error(`No reply to "${command}"`));
			}, timeout);
			this.active = active;

			Promise.resolve(this.send(`${CLEAR_LINE}${command}\r`)).catch((err) => {
				this.finish(active, err);
			});
		}));

		this.queue = result.catch(() => {});
		return result;
	}

	/**
	 * Sends a command that ends the connection (sleep, bootloader), so no
	 * reply is awaited.
	 */
	fire(command) {
		const result = this.queue.then(() => this.send(`${CLEAR_LINE}${command}\r`));

		this.queue = result.catch(() => {});
		return result;
	}

	/** Call when the link drops: fails whatever is waiting for a reply. */
	reset() {
		if (this.active) {
			this.finish(this.active, new Error("Disconnected"));
		}
		this.decoder = new TextDecoder();
	}
}

// ---------------------------------------------------------- Reply parsers ---

export function parseBlink(lines) {
	const match = /^rate (\d+) ms, pattern (\S+)$/.exec(lines[0] || "");

	if (!match) {
		throw new Error("Unexpected reply to blink");
	}
	return { rate: Number(match[1]), pattern: match[2] };
}

export function parseImu(lines) {
	const sensor = (name) => {
		const line = lines.find((l) => l.startsWith(name)) || "";
		const match = /odr (\S+) Hz, range \+-(\d+) \w+, ([\w-]+) mode/.exec(line);

		if (!match) {
			throw new Error("Unexpected reply to imu");
		}
		return { odr: match[1], range: match[2], mode: match[3] === "low-power" ? "low" : "high" };
	};

	return {
		accel: sensor("accel"),
		gyro: sensor("gyro"),
		saved: (lines.find((l) => l.startsWith("saved")) || "").includes("yes"),
	};
}

export function parseImuRead(lines) {
	const axes = (name) => {
		const line = lines.find((l) => l.startsWith(name)) || "";
		const match = /x=(-?[\d.]+) y=(-?[\d.]+) z=(-?[\d.]+)/.exec(line);

		if (!match) {
			throw new Error("Unexpected reply to imu read");
		}
		return match.slice(1).map(Number);
	};
	const temp = /^temp\s+(-?[\d.]+)/.exec(lines.find((l) => l.startsWith("temp")) || "");

	return { accel: axes("accel"), gyro: axes("gyro"), temp: temp ? Number(temp[1]) : null };
}

export function parseBonds(lines) {
	const summary = /^(\d+) of (\d+) bonds used, pairing (on|off)/.exec(lines.at(-1) || "");

	if (!summary) {
		throw new Error("Unexpected reply to bonds");
	}

	const bonds = lines.slice(0, -1).map((line) => {
		const match = /^(\d+)\s+(\S+ \(\w+\))(\s+\(connected\))?$/.exec(line);

		if (!match) {
			throw new Error("Unexpected reply to bonds");
		}
		return { index: Number(match[1]), address: match[2], connected: Boolean(match[3]) };
	});

	return {
		bonds,
		used: Number(summary[1]),
		max: Number(summary[2]),
		pairing: summary[3] === "on",
	};
}

export function parseHid(lines) {
	const find = (pattern) => lines.map((line) => pattern.exec(line)).find(Boolean);
	const list = (text) => (text.trim() === "none" ? [] : text.trim().split(" "));
	const enabled = find(/^enabled:(.*)$/);
	const hosts = find(/^hosts (\d+), listening to:(.*)$/);
	const rate = find(/^sensor rate (\d+) Hz$/);

	// Absent on firmware from before the air mouse.
	const air = find(
		/^air mouse (on|off), speed (\d+) (\d+), accel (\d+) deg\/s, dead zone (\d+) deg\/s, axes (?:auto|(\S+) (\S+)), forward (\S+)$/);

	if (!enabled || !hosts || !rate) {
		throw new Error("Unexpected reply to hid");
	}
	return {
		air: air && {
			on: air[1] === "on",
			speedX: Number(air[2]),
			speedY: Number(air[3]),
			accel: Number(air[4]),
			deadzone: Number(air[5]),
			auto: !air[6],
			axisX: air[6] || null,
			axisY: air[7] || null,
			forward: air[8],
		},
		enabled: list(enabled[1]),
		hosts: Number(hosts[1]),
		listening: list(hosts[2]),
		rate: Number(rate[1]),
	};
}
