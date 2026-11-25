// SPDX-License-Identifier: MIT
//
// Orientation viewer: draws the board as a 3D slab that turns with the real
// one. The board notifies its fused orientation as a quaternion 50 times a
// second (see "orientation stream" in src/hid.c); it is only asked to while
// the viewer's card is open.

export const ORIENTATION_SERVICE = "7c3b0001-5f1e-4c8a-9d2e-3a6b8f1c2d4e";
export const ORIENTATION_VALUE = "7c3b0002-5f1e-4c8a-9d2e-3a6b8f1c2d4e";

const $ = (id) => document.getElementById(id);
const DEG = 180 / Math.PI;
const RETRY_MS = 300;
const RETRIES = 5;

/** Decodes a notification: x, y, z, w as int16, scaled by 10000. Null if malformed. */
export function decodeQuaternion(view) {
	if (view.byteLength < 8) {
		return null;
	}

	const q = [0, 2, 4, 6].map((offset) => view.getInt16(offset, true) / 10000);
	const norm = Math.hypot(...q);

	return norm > 0.5 ? q.map((value) => value / norm) : null;
}

/** Heading: the turn about the vertical, in radians. */
export function yawOf([x, y, z, w]) {
	return Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
}

/** The same orientation seen from a heading turned by -yaw about the vertical. */
export function withoutYaw([x, y, z, w], yaw) {
	const c = Math.cos(-yaw / 2);
	const s = Math.sin(-yaw / 2);

	// (0, 0, s, c) * q
	return [c * x - s * y, c * y + s * x, c * z + s * w, c * w - s * z];
}

/** Rotation matrix (rows), board axes to room axes with Z up. */
export function rotationMatrix([x, y, z, w]) {
	return [
		[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
		[2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
		[2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
	];
}

/** Roll, pitch and yaw in degrees (rotations about the board's X, Y and the vertical). */
export function eulerDegrees(q) {
	const [x, y, z, w] = q;
	const sinPitch = Math.max(-1, Math.min(1, 2 * (w * y - z * x)));

	return {
		roll: Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * DEG,
		pitch: Math.asin(sinPitch) * DEG,
		yaw: yawOf(q) * DEG,
	};
}

/**
 * CSS transform for a rotation. The screen's axes are X right, Y down and Z
 * towards the viewer; the room is drawn with X right, Y into the screen and Z
 * up, so the matrix is the rotation with its Y and Z axes swapped and negated.
 */
export function cssMatrix(r) {
	const m = [
		[r[0][0], -r[0][2], -r[0][1]],
		[-r[2][0], r[2][2], r[2][1]],
		[-r[1][0], r[1][2], r[1][1]],
	];

	// matrix3d() takes its values column by column.
	return `matrix3d(${m[0][0]},${m[1][0]},${m[2][0]},0,` +
		`${m[0][1]},${m[1][1]},${m[2][1]},0,` +
		`${m[0][2]},${m[1][2]},${m[2][2]},0,0,0,0,1)`;
}

export function initOrientation() {
	const card = document.querySelector('[data-card="orientation"]');
	const model = $("orientation-model");
	const note = $("orientation-note");
	let source = null; // the characteristic, while connected to firmware that has it
	let supported = true;
	let streaming = false;
	let wanted = card.classList.contains("open");
	let latest = null;
	let zeroYaw = null;
	let frame = 0;
	let count = 0;
	let countSince = performance.now();

	function draw() {
		frame = 0;
		if (!latest) {
			return;
		}
		if (zeroYaw === null) {
			zeroYaw = yawOf(latest);
		}

		const q = withoutYaw(latest, zeroYaw);
		const { roll, pitch, yaw } = eulerDegrees(q);

		model.style.transform = cssMatrix(rotationMatrix(q));
		$("orientation-roll").textContent = `${roll.toFixed(0)}°`;
		$("orientation-pitch").textContent = `${pitch.toFixed(0)}°`;
		$("orientation-yaw").textContent = `${yaw.toFixed(0)}°`;
	}

	function onValue(event) {
		const q = decodeQuaternion(event.target.value);
		const now = performance.now();

		if (!q) {
			return;
		}
		latest = q;
		count++;
		if (now - countSince >= 1000) {
			$("orientation-rate").textContent = `${Math.round(count * 1000 / (now - countSince))} Hz`;
			count = 0;
			countSince = now;
		}
		// At most one redraw per screen frame, however fast the samples arrive.
		if (!frame) {
			frame = requestAnimationFrame(draw);
		}
	}

	function showNote() {
		note.textContent = !supported
			? "This board's firmware has no orientation stream. Upgrade it to use the viewer."
			: !source
				? "Connect to see the board's orientation."
				: "Heading has no fixed reference: it starts from where the board was " +
				  "and drifts slowly. Face forward sets it back to straight ahead.";
	}

	// Another Bluetooth operation, such as a console command, may be under way:
	// the browser then refuses this one, so it is tried again a few times.
	async function attempt(operation) {
		for (let tries = 0; ; tries++) {
			try {
				return await operation();
			} catch (err) {
				if (tries >= RETRIES) {
					throw err;
				}
				await new Promise((resolve) => setTimeout(resolve, RETRY_MS));
			}
		}
	}

	let syncing = Promise.resolve();

	// Starts or stops the stream to match: wanted while the card is open.
	function sync() {
		syncing = syncing.then(async () => {
			const target = source;

			if (!target || streaming === wanted) {
				return;
			}
			try {
				if (wanted) {
					await attempt(() => target.startNotifications());
					count = 0;
					countSince = performance.now();
				} else {
					await attempt(() => target.stopNotifications());
					$("orientation-rate").textContent = "";
				}
				if (source === target) {
					streaming = wanted;
				}
			} catch (err) {
				if (source === target) {
					note.textContent = `Could not ${wanted ? "start" : "stop"} the stream: ${err.message}`;
				}
			}
		});
		return syncing;
	}

	new MutationObserver(() => {
		const open = card.classList.contains("open");

		if (open !== wanted) {
			wanted = open;
			sync();
		}
	}).observe(card, { attributes: true, attributeFilter: ["class"] });

	$("orientation-zero").addEventListener("click", () => {
		zeroYaw = null;
		if (latest && !frame) {
			frame = requestAnimationFrame(draw);
		}
	});

	showNote();

	return {
		/**
		 * Call with the orientation characteristic after connecting (null if
		 * the firmware has none), and with nothing after disconnecting.
		 */
		setSource(characteristic, connected = Boolean(characteristic)) {
			if (source) {
				source.removeEventListener("characteristicvaluechanged", onValue);
			}
			source = characteristic || null;
			supported = !connected || Boolean(characteristic);
			streaming = false;
			latest = null;
			zeroYaw = null;
			$("orientation-zero").disabled = !source;
			$("orientation-rate").textContent = "";
			if (source) {
				source.addEventListener("characteristicvaluechanged", onValue);
				sync();
			}
			showNote();
		},
	};
}
