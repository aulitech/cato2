// SPDX-License-Identifier: MIT
//
// MCUmgr Simple Management Protocol (SMP) helpers: packet framing, the small
// subset of CBOR that SMP uses, and reading firmware files. No browser APIs
// beyond DecompressionStream, so this module also runs under Node for testing.

export const OP_READ = 0;
export const OP_WRITE = 2;

export const GROUP_OS = 0;
export const GROUP_IMAGE = 1;
export const GROUP_FS = 8;

export const OS_RESET = 5;
export const IMAGE_STATE = 0;
export const IMAGE_UPLOAD = 1;
export const FS_FILE = 0; // write: a piece of an upload; read: a piece of a download

export const HEADER_SIZE = 8;

const MCUBOOT_MAGIC = 0x96f3b83d;

// ---------------------------------------------------------------- CBOR ----

const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder();

function cborHead(major, value, out) {
	const m = major << 5;

	if (value < 24) {
		out.push(m | value);
	} else if (value < 0x100) {
		out.push(m | 24, value);
	} else if (value < 0x10000) {
		out.push(m | 25, value >> 8, value & 0xff);
	} else if (value < 0x100000000) {
		out.push(m | 26, (value >>> 24) & 0xff, (value >>> 16) & 0xff,
			 (value >>> 8) & 0xff, value & 0xff);
	} else {
		throw new RangeError("CBOR integer too large");
	}
}

function cborEncodeInto(value, out) {
	if (typeof value === "number") {
		if (!Number.isInteger(value)) {
			throw new TypeError("CBOR floats are not supported");
		}
		if (value >= 0) {
			cborHead(0, value, out);
		} else {
			cborHead(1, -1 - value, out);
		}
	} else if (typeof value === "boolean") {
		out.push(value ? 0xf5 : 0xf4);
	} else if (typeof value === "string") {
		const bytes = utf8Encoder.encode(value);

		cborHead(3, bytes.length, out);
		for (const b of bytes) {
			out.push(b);
		}
	} else if (value instanceof Uint8Array) {
		cborHead(2, value.length, out);
		for (const b of value) {
			out.push(b);
		}
	} else if (Array.isArray(value)) {
		cborHead(4, value.length, out);
		for (const item of value) {
			cborEncodeInto(item, out);
		}
	} else if (value !== null && typeof value === "object") {
		const keys = Object.keys(value);

		cborHead(5, keys.length, out);
		for (const key of keys) {
			cborEncodeInto(key, out);
			cborEncodeInto(value[key], out);
		}
	} else {
		throw new TypeError(`Cannot CBOR-encode ${value}`);
	}
}

export function cborEncode(value) {
	const out = [];

	cborEncodeInto(value, out);
	return new Uint8Array(out);
}

export function cborDecode(bytes) {
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	let pos = 0;

	const isBreak = () => bytes[pos] === 0xff;

	function argument(info) {
		if (info < 24) {
			return info;
		}
		if (info === 24) {
			return view.getUint8(pos++);
		}
		if (info === 25) {
			pos += 2;
			return view.getUint16(pos - 2);
		}
		if (info === 26) {
			pos += 4;
			return view.getUint32(pos - 4);
		}
		if (info === 27) {
			pos += 8;
			return Number(view.getBigUint64(pos - 8));
		}
		throw new Error("Unsupported CBOR length encoding");
	}

	function item() {
		if (pos >= bytes.length) {
			throw new Error("Truncated CBOR data");
		}

		const initial = bytes[pos++];
		const major = initial >> 5;
		const info = initial & 0x1f;
		const indefinite = info === 31;

		switch (major) {
		case 0:
			return argument(info);
		case 1:
			return -1 - argument(info);
		case 2:
		case 3: {
			const len = argument(info);
			const chunk = bytes.subarray(pos, pos + len);

			pos += len;
			return major === 2 ? chunk.slice() : utf8Decoder.decode(chunk);
		}
		case 4: {
			const list = [];
			const len = indefinite ? Infinity : argument(info);

			for (let i = 0; i < len && !(indefinite && isBreak()); i++) {
				list.push(item());
			}
			pos += indefinite ? 1 : 0;
			return list;
		}
		case 5: {
			const map = {};
			const len = indefinite ? Infinity : argument(info);

			for (let i = 0; i < len && !(indefinite && isBreak()); i++) {
				const key = item();

				map[key] = item();
			}
			pos += indefinite ? 1 : 0;
			return map;
		}
		case 7:
			if (info === 20) {
				return false;
			}
			if (info === 21) {
				return true;
			}
			if (info === 22 || info === 23) {
				return null;
			}
			if (info === 26) {
				pos += 4;
				return view.getFloat32(pos - 4);
			}
			if (info === 27) {
				pos += 8;
				return view.getFloat64(pos - 8);
			}
			break;
		}
		throw new Error(`Unsupported CBOR item 0x${initial.toString(16)}`);
	}

	return item();
}

// ----------------------------------------------------------- SMP frames ----

export function encodePacket(op, group, id, seq, payload = {}) {
	const body = cborEncode(payload);
	const packet = new Uint8Array(HEADER_SIZE + body.length);
	const view = new DataView(packet.buffer);

	view.setUint8(0, op);
	view.setUint8(1, 0);
	view.setUint16(2, body.length);
	view.setUint16(4, group);
	view.setUint8(6, seq & 0xff);
	view.setUint8(7, id);
	packet.set(body, HEADER_SIZE);

	return packet;
}

/**
 * Collects notification fragments into whole SMP packets. Feed each
 * notification to push(); it returns a decoded packet once one is complete,
 * otherwise null.
 */
export class PacketAssembler {
	constructor() {
		this.buffer = new Uint8Array(0);
	}

	reset() {
		this.buffer = new Uint8Array(0);
	}

	push(fragment) {
		const joined = new Uint8Array(this.buffer.length + fragment.length);

		joined.set(this.buffer);
		joined.set(fragment, this.buffer.length);
		this.buffer = joined;

		if (this.buffer.length < HEADER_SIZE) {
			return null;
		}

		const view = new DataView(this.buffer.buffer);
		const total = HEADER_SIZE + view.getUint16(2);

		if (this.buffer.length < total) {
			return null;
		}

		const packet = {
			op: view.getUint8(0) & 0x07,
			group: view.getUint16(4),
			seq: view.getUint8(6),
			id: view.getUint8(7),
			payload: cborDecode(this.buffer.subarray(HEADER_SIZE, total)),
		};

		this.buffer = this.buffer.slice(total);
		return packet;
	}
}

/** Error code from a response payload, or 0 if the request succeeded. */
export function responseError(payload) {
	if (payload.err) {
		return payload.err.rc || -1;
	}
	return payload.rc || 0;
}

const RC_NAMES = {
	1: "unknown error",
	2: "out of memory",
	3: "invalid value",
	4: "timeout",
	5: "no such entry",
	6: "bad state",
	7: "response too large",
	8: "not supported",
	9: "corrupt payload",
	10: "busy",
	11: "access denied",
};

export function describeError(rc) {
	return RC_NAMES[rc] ? `${RC_NAMES[rc]} (rc ${rc})` : `rc ${rc}`;
}

/**
 * Builds one image upload request that fits in maxPacket bytes, carrying as
 * much of the image as possible from the given offset.
 */
export function encodeUploadChunk(image, sha, offset, seq, maxPacket) {
	const fields = { off: offset };

	if (offset === 0) {
		fields.image = 0;
		fields.len = image.length;
		fields.sha = sha;
	}

	// Map header and "data" key are already counted with an empty payload;
	// reserve two more bytes for the byte string's longer length prefix.
	const overhead = HEADER_SIZE + cborEncode({ data: new Uint8Array(0), ...fields }).length + 2;
	const room = maxPacket - overhead;

	if (room < 1) {
		throw new RangeError("Packet size too small for an upload chunk");
	}

	const data = image.subarray(offset, Math.min(image.length, offset + room));

	return {
		packet: encodePacket(OP_WRITE, GROUP_IMAGE, IMAGE_UPLOAD, seq, { data, ...fields }),
		next: offset + data.length,
	};
}

// ------------------------------------------------------- Firmware files ----

export function toHex(bytes) {
	return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

/** Reads the MCUboot header of a signed image; throws if it is not one. */
export function parseImageHeader(image) {
	if (image.length < 32) {
		throw new Error("File is too small to be a firmware image");
	}

	const view = new DataView(image.buffer, image.byteOffset, image.byteLength);

	if (view.getUint32(0, true) !== MCUBOOT_MAGIC) {
		throw new Error("Not an MCUboot image (use zephyr.signed.bin or cato.zip)");
	}

	const major = view.getUint8(20);
	const minor = view.getUint8(21);
	const revision = view.getUint16(22, true);
	const build = view.getUint32(24, true);

	return {
		version: `${major}.${minor}.${revision}` + (build ? `+${build}` : ""),
		size: image.length,
		hash: imageHash(image, view),
	};
}

/**
 * The image's SHA-256, which is how the board identifies an image. It is in
 * the list of tagged values that follows the code. Null if it is not found.
 */
function imageHash(image, view) {
	const TLV_INFO = 0x6907;
	const TLV_PROTECTED_INFO = 0x6908;
	const TLV_SHA256 = 0x10;
	let offset = view.getUint16(8, true) + view.getUint32(12, true);

	if (offset + 4 <= image.length && view.getUint16(offset, true) === TLV_PROTECTED_INFO) {
		offset += view.getUint16(offset + 2, true);
	}
	if (offset + 4 > image.length || view.getUint16(offset, true) !== TLV_INFO) {
		return null;
	}

	const end = Math.min(image.length, offset + view.getUint16(offset + 2, true));

	offset += 4;
	while (offset + 4 <= end) {
		const type = view.getUint16(offset, true);
		const length = view.getUint16(offset + 2, true);

		if (type === TLV_SHA256 && offset + 4 + length <= end) {
			return image.slice(offset + 4, offset + 4 + length);
		}
		offset += 4 + length;
	}
	return null;
}

async function inflateRaw(data) {
	const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw"));

	return new Uint8Array(await new Response(stream).arrayBuffer());
}

/** Returns the entries of a zip archive as { name: Uint8Array }. */
export async function unzip(archive) {
	const view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
	let end = archive.length - 22;

	while (end >= 0 && view.getUint32(end, true) !== 0x06054b50) {
		end--;
	}
	if (end < 0) {
		throw new Error("Not a zip archive");
	}

	const count = view.getUint16(end + 10, true);
	let entry = view.getUint32(end + 16, true);
	const files = {};

	for (let i = 0; i < count; i++) {
		if (view.getUint32(entry, true) !== 0x02014b50) {
			throw new Error("Corrupt zip archive");
		}

		const method = view.getUint16(entry + 10, true);
		const compressedSize = view.getUint32(entry + 20, true);
		const nameLen = view.getUint16(entry + 28, true);
		const extraLen = view.getUint16(entry + 30, true);
		const commentLen = view.getUint16(entry + 32, true);
		const local = view.getUint32(entry + 42, true);
		const name = utf8Decoder.decode(archive.subarray(entry + 46, entry + 46 + nameLen));

		const dataStart = local + 30 + view.getUint16(local + 26, true) +
				  view.getUint16(local + 28, true);
		const data = archive.subarray(dataStart, dataStart + compressedSize);

		if (method === 0) {
			files[name] = data.slice();
		} else if (method === 8) {
			files[name] = await inflateRaw(data);
		} else {
			throw new Error(`Unsupported zip compression method ${method}`);
		}

		entry += 46 + nameLen + extraLen + commentLen;
	}

	return files;
}

/**
 * Extracts the application image from a firmware file: either a signed
 * MCUboot .bin, or the cato.zip produced by the build.
 */
export async function loadFirmware(fileName, bytes) {
	let image = bytes;
	let name = fileName;
	let loadAddress = null; // where in flash the image runs from, if the file says

	if (bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b) {
		const files = await unzip(bytes);
		const manifestBytes = files["manifest.json"];

		if (!manifestBytes) {
			throw new Error("Zip archive has no manifest.json");
		}

		const manifest = JSON.parse(utf8Decoder.decode(manifestBytes));
		const apps = (manifest.files || []).filter((f) => f.type === "application");

		if (apps.length !== 1) {
			throw new Error("Expected exactly one application image in the archive");
		}

		name = apps[0].file;
		if (Number.isInteger(apps[0].load_address)) {
			loadAddress = apps[0].load_address;
		}
		image = files[name];
		if (!image) {
			throw new Error(`Archive is missing ${name}`);
		}
	}

	return { name, image, loadAddress, ...parseImageHeader(image) };
}
