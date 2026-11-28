// SPDX-License-Identifier: MIT
//
// The Files tab: the files on the board's flash (see src/files.c), listed,
// sent, fetched and removed over Bluetooth.
//
// The list, and everything that changes a file, goes through the board's
// "files" commands; the contents travel by MCUmgr's file transfer, a piece at
// a time. The board may be in use by others meanwhile (another MyCato, the USB
// console, the firmware itself), so nothing here trusts a transfer to have
// gone undisturbed:
//
//  - One thing at a time. A transfer reserves the link with the page's other
//    work (firmware upgrades included); the board refuses what would collide
//    with someone else's, and that is reported, not retried blindly.
//  - A file is sent under a temporary name. The board makes it the file only
//    once its size and CRC-32 are what was sent, and at a stroke: a transfer
//    that is cut short, or mixed up with another's, leaves the file as it was.
//  - A file is fetched against the size and CRC-32 the board gives for it
//    first. What arrives has to match, or it is fetched again, and failing
//    that not handed over.
//  - An upload that did not make it is discarded on the board, which also
//    clears such leftovers itself when it starts.

import { ask } from "./dialog.js";
import { ShellError } from "./shell.js";
import * as smp from "./smp.js";

const $ = (id) => document.getElementById(id);

const MOUNT = "/lfs/";
const PART = ".part";
const NAME_MAX = 40; // as in src/files.c
const HEADROOM = 2 * 4096; // flash blocks a new file needs beyond its bytes
const PACKET = 244; // one write on a 247-byte MTU link, as for firmware
const DISCARD_TRIES = 4; // the board holds an interrupted upload for a few seconds
const DISCARD_WAIT_MS = 2500;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** Thrown to leave an action the user decided against: not a failure. */
class Cancelled extends Error {}

const CRC_TABLE = Uint32Array.from({ length: 256 }, (_, n) => {
	let c = n;

	for (let k = 0; k < 8; k++) {
		c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
	}
	return c >>> 0;
});

/** CRC-32 (IEEE), as the board computes it. */
export function crc32(bytes) {
	let crc = 0xffffffff;

	for (let i = 0; i < bytes.length; i++) {
		crc = CRC_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
	}
	return (crc ^ 0xffffffff) >>> 0;
}

/** Whether the board takes this as a file's name. */
export function nameValid(name) {
	return name.length <= NAME_MAX && /^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name) &&
		!/\.(part|tmp)$/.test(name);
}

/** The nearest name the board takes: its other characters become "_". */
export function boardName(name) {
	let base = name.normalize("NFKD").replace(/[^A-Za-z0-9._-]/g, "_").replace(/^[^A-Za-z0-9]+/, "");
	const dot = base.lastIndexOf(".");
	const extension = dot > 0 && base.length - dot <= 8 ? base.slice(dot) : "";

	// Too long: shortened ahead of the extension, which says what the file is.
	if (base.length > NAME_MAX) {
		base = base.slice(0, NAME_MAX - extension.length).replace(/\.+$/, "") + extension;
	}
	if (/\.(part|tmp)$/.test(base)) {
		base = `${base.slice(0, NAME_MAX - 1)}_`;
	}
	return base || "file";
}

export function parseList(lines) {
	const summary = /^(\d+) files, (\d+) of (\d+) bytes free$/.exec(lines.at(-1) || "");

	if (!summary) {
		throw new Error("Unexpected reply to files");
	}
	return {
		files: lines.slice(0, -1).map((line) => {
			const match = /^(\d+)\s+(\S+)$/.exec(line);

			if (!match) {
				throw new Error("Unexpected reply to files");
			}
			return { name: match[2], size: Number(match[1]) };
		}),
		free: Number(summary[2]),
		total: Number(summary[3]),
	};
}

function formatSize(bytes) {
	if (bytes < 1024) {
		return `${bytes} B`;
	}
	if (bytes < 1024 * 1024) {
		return `${(bytes / 1024).toFixed(bytes < 10240 ? 1 : 0)} kB`;
	}
	return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}

/**
 * @param shell      runs the board's commands
 * @param request    (op, group, id, payload) => promise of an SMP reply
 * @param exclusive  (task) => runs an async task with the link reserved for
 *                   it, or throws if something else has it
 */
export function initFiles({ shell, request, exclusive }) {
	const list = $("files-list");
	const message = $("files-message");
	const progress = $("files-progress");
	const picker = $("files-pick");
	let listing = null; // the board's files as last read, null when not known
	let connected = false;

	function say(text = "", isError = false) {
		message.textContent = text;
		message.classList.toggle("error", isError);
	}

	function showProgress(fraction, text) {
		progress.hidden = fraction === null;
		if (fraction !== null) {
			$("files-progress-bar").value = fraction;
			$("files-progress-text").textContent = text;
		}
	}

	function transferProgress(what, done, total, started) {
		const seconds = Math.max((performance.now() - started) / 1000, 0.1);

		showProgress(done / total, `${what} · ${Math.round((done / total) * 100)}% · ` +
					   `${(done / 1024 / seconds).toFixed(1)} kB/s`);
	}

	/** What went wrong, in the user's terms. */
	function explain(err) {
		if (/\(rc 10\)/.test(err.message)) {
			return "The board is busy with another file transfer. Try again in a moment.";
		}
		if (/\(rc 11\)/.test(err.message)) {
			return "The board does not allow that file to be transferred.";
		}
		if (err instanceof ShellError && /not found/i.test(err.message)) {
			return "This board's firmware has no file storage. Upgrade it to use Files.";
		}
		return err.message;
	}

	function render() {
		list.replaceChildren();
		$("files-space").textContent = listing
			? `${formatSize(listing.free)} free of ${formatSize(listing.total)}`
			: "";

		if (!listing || !listing.files.length) {
			const cell = list.insertRow().insertCell();

			cell.colSpan = 3;
			cell.textContent = !connected ? "Connect to see the board's files."
				: listing ? "No files yet." : "";
			return;
		}

		for (const file of [...listing.files].sort((a, b) => a.name.localeCompare(b.name))) {
			const row = list.insertRow();
			const actions = document.createElement("span");
			const download = document.createElement("button");
			const remove = document.createElement("button");

			row.insertCell().textContent = file.name;
			row.insertCell().textContent = formatSize(file.size);
			download.className = "small";
			download.textContent = "Download";
			download.addEventListener("click", () => downloadFile(file.name));
			remove.className = "small danger";
			remove.textContent = "Delete";
			remove.addEventListener("click", () => removeFile(file.name));
			actions.className = "row";
			actions.append(download, remove);
			row.insertCell().append(actions);
		}
	}

	async function load() {
		listing = parseList(await shell.run("files"));
		render();
	}

	/** Runs one of the tab's actions with the link reserved, and reports how it went. */
	async function act(task, done = "") {
		say();
		try {
			await exclusive(async () => {
				try {
					await task();
				} finally {
					showProgress(null);
					// Whatever happened, show the board as it now is.
					await load().catch(() => {});
				}
			});
			say(done);
		} catch (err) {
			if (err instanceof Cancelled) {
				say();
			} else if (connected) {
				say(explain(err), true);
			} else {
				say("The connection was lost. Nothing is left half done on the board: " +
				    "check the list after reconnecting.", true);
			}
		}
	}

	// ------------------------------------------------------------- send ---

	async function send(name, bytes) {
		const path = MOUNT + name + PART;
		const started = performance.now();
		let offset = 0;

		while (offset < bytes.length) {
			const fields = { off: offset, name: path };

			if (offset === 0) {
				fields.len = bytes.length;
			}

			// As much as fits in one packet beside the other fields.
			const room = PACKET - smp.HEADER_SIZE -
				smp.cborEncode({ data: new Uint8Array(0), ...fields }).length - 2;
			const data = bytes.subarray(offset, offset + room);
			const reply = await request(smp.OP_WRITE, smp.GROUP_FS, smp.FS_FILE, { data, ...fields });

			if (typeof reply.off !== "number") {
				throw new Error("The board sent an unexpected reply to the upload");
			}
			// Anything but straight on means this is not the only transfer
			// writing the file: stop, rather than carry on into a mix.
			if (reply.off !== offset + data.length) {
				throw new Error("The upload was disturbed by another transfer and was abandoned.");
			}
			offset = reply.off;
			transferProgress(`Sending ${name}`, Math.min(offset, bytes.length), bytes.length, started);
		}
	}

	/** Best effort: the board also clears leftovers when it starts. */
	async function discard(name) {
		for (let tries = 0; connected && tries < DISCARD_TRIES; tries++) {
			try {
				await shell.run(`files discard ${name}`, { quiet: true });
				return;
			} catch {
				// Still held by the board as arriving: it lets go shortly.
				await sleep(DISCARD_WAIT_MS);
			}
		}
	}

	async function uploadFile(file) {
		const name = boardName(file.name);

		if (!file.size) {
			say(`${file.name} is empty: there is nothing to send.`, true);
			return;
		}

		await act(async () => {
			// The board as it is now, not as it was when the list was drawn.
			await load();

			const existing = listing.files.find((entry) => entry.name === name);

			if (file.size + HEADROOM > listing.free) {
				throw new Error(`Not enough room: ${file.name} is ${formatSize(file.size)} and ` +
						`the board has ${formatSize(listing.free)} free.`);
			}
			if (existing || name !== file.name) {
				const question = existing ? `Replace ${name} on the board?` : `Send ${file.name}?`;
				const details = [
					name !== file.name
						? `The board's file names are plain: it will be stored as ${name}.` : "",
					existing ? `The file there now (${formatSize(existing.size)}) will be lost.` : "",
				].filter(Boolean).join(" ");

				if (!await ask(`${question}\n\n${details}`,
					       { action: existing ? "Replace" : "Send", danger: Boolean(existing) })) {
					throw new Cancelled();
				}
			}

			const bytes = new Uint8Array(await file.arrayBuffer());
			const crc = crc32(bytes).toString(16).padStart(8, "0");

			try {
				await send(name, bytes);
				showProgress(1, `Checking ${name}…`);
				// The board compares, and only then puts the file in place.
				await shell.run(`files commit ${name} ${bytes.length} ${crc}`);
			} catch (err) {
				await discard(name);
				throw err;
			}
		}, `${name} is on the board.`);
	}

	// ------------------------------------------------------------ fetch ---

	async function fetchOnce(name) {
		const reply = await shell.run(`files crc ${name}`, { quiet: true });
		const match = /^(\d+) bytes, crc32 ([0-9a-f]{8})$/.exec(reply.at(-1) || "");

		if (!match) {
			throw new Error("Unexpected reply to files crc");
		}

		const size = Number(match[1]);
		const bytes = new Uint8Array(size);
		const started = performance.now();
		let offset = 0;

		while (offset < size) {
			const piece = await request(smp.OP_READ, smp.GROUP_FS, smp.FS_FILE,
						    { off: offset, name: MOUNT + name });

			if (!(piece.data instanceof Uint8Array) || piece.off !== offset ||
			    !piece.data.length || offset + piece.data.length > size) {
				// Shorter, longer or elsewhere than it was a moment ago.
				return null;
			}
			bytes.set(piece.data, offset);
			offset += piece.data.length;
			transferProgress(`Fetching ${name}`, offset, size, started);
		}

		return crc32(bytes).toString(16).padStart(8, "0") === match[2] ? bytes : null;
	}

	async function downloadFile(name) {
		await act(async () => {
			// A file replaced while it was on its way does not match the
			// checksum taken first: one more go, from the start.
			const bytes = await fetchOnce(name) || await fetchOnce(name);

			if (!bytes) {
				throw new Error(`${name} kept changing on the board while it was being ` +
						"fetched, so it was not saved. Try again.");
			}

			const link = document.createElement("a");

			link.href = URL.createObjectURL(new Blob([bytes]));
			link.download = name;
			link.click();
			setTimeout(() => URL.revokeObjectURL(link.href), 60000);
		}, `${name} was downloaded.`);
	}

	// ----------------------------------------------------------- remove ---

	async function removeFile(name) {
		if (!await ask(`Delete ${name} from the board?\n\nIt cannot be brought back.`,
			       { action: "Delete", danger: true })) {
			return;
		}
		await act(() => shell.run(`files rm ${name}`), `${name} was deleted.`);
	}

	// --------------------------------------------------------------- UI ---

	$("files-upload").addEventListener("click", () => picker.click());
	picker.addEventListener("change", async () => {
		const file = picker.files[0];

		picker.value = ""; // the same file can be picked again
		if (file) {
			await uploadFile(file);
		}
	});
	$("files-refresh").addEventListener("click", () => act(async () => {}));

	render();

	return {
		/** Call when the board connects and when it disconnects. */
		setConnected(value) {
			if (value === connected) {
				return;
			}
			connected = value;
			if (connected) {
				say();
				load().catch((err) => say(explain(err), true));
			} else {
				listing = null;
				render();
			}
		},
	};
}
