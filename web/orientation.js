// SPDX-License-Identifier: MIT
//
// Orientation viewer: draws the Cato in 3D, turning with the real one (its
// case from an STL model, see model.js; a plain slab if that cannot be drawn).
// The viewer is a window of its own (orientation.html), opened from the app's
// top row. The board notifies its fused orientation as a quaternion 50 times a
// second (see "orientation stream" in src/hid.c); it is only asked to while
// that window is open, and the app's page passes each one on to it.
//
// Around the board is a screen: where the board believes the screen it points
// at to be, which the pointer (air mouse) needs to know. The user moves it to
// the side of the board the real screen is on.

import { createModelView, parseStl } from "./model.js";

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
export function cssAxes(r) {
	return [
		[r[0][0], -r[0][2], -r[0][1]],
		[-r[2][0], r[2][2], r[2][1]],
		[-r[1][0], r[1][2], r[1][1]],
	];
}

export function cssMatrix(r) {
	const m = cssAxes(r);

	// matrix3d() takes its values column by column.
	return `matrix3d(${m[0][0]},${m[1][0]},${m[2][0]},0,` +
		`${m[0][1]},${m[1][1]},${m[2][1]},0,` +
		`${m[0][2]},${m[1][2]},${m[2][2]},0,0,0,0,1)`;
}

/**
 * The inverse of cssMatrix(r) followed by the inverse of the camera's tilt:
 * a child of the model with this transform faces the viewer squarely.
 */
export function cssFacingViewer(r, cameraTilt) {
	const m = [
		[r[0][0], -r[0][2], -r[0][1]],
		[-r[2][0], r[2][2], r[2][1]],
		[-r[1][0], r[1][2], r[1][1]],
	];

	// The transpose of a rotation is its inverse.
	return `matrix3d(${m[0][0]},${m[0][1]},${m[0][2]},0,` +
		`${m[1][0]},${m[1][1]},${m[1][2]},0,` +
		`${m[2][0]},${m[2][1]},${m[2][2]},0,0,0,0,1) rotateX(${-cameraTilt}deg)`;
}

const CAMERA_TILT = -24; // degrees, as in the stylesheet
const PERSPECTIVE = 700; // pixels, as in the stylesheet

/** The scene's axes as the tilted camera sees them: CSS rotateX(CAMERA_TILT). */
function tilted(m) {
	const c = Math.cos(CAMERA_TILT * Math.PI / 180);
	const s = Math.sin(CAMERA_TILT * Math.PI / 180);

	return [
		m[0],
		m[1].map((value, i) => c * value - s * m[2][i]),
		m[1].map((value, i) => s * value + c * m[2][i]),
	];
}
const IDENTITY = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];

// Where the screen is drawn for each board axis that can point at it, in the
// model's own space (X right, Y down, Z towards the viewer; see cssMatrix).
const SCREEN_SPOTS = {
	"+x": [120, 0, 0],
	"-x": [-120, 0, 0],
	"+y": [0, 0, -115],
	"-y": [0, 0, 115],
	"+z": [0, -70, 0],
	"-z": [0, 70, 0],
};

/**
 * The viewer itself, for the page that shows it (orientation.html). It is
 * told what to draw: it has no link to the board.
 *
 * @param setForward  (axis) => void, asks for the screen to be moved to the
 *                    side of the board that axis points out of
 * @returns { sample(q), setState({ live, supported, forward }), setNote(text) }
 */
export function initOrientationView({ setForward }) {
	const model = $("orientation-model");
	const note = $("orientation-note");
	let live = false; // connected to a board whose firmware has the stream
	let supported = true;
	let latest = null;
	let zeroYaw = null;
	let frame = 0;
	let count = 0;
	let countSince = performance.now();
	let forward = null; // the axis pointing at the screen, once the board has said
	const spots = {};

	// The case, drawn on a canvas under the screen and its dots. Until (and
	// unless) it is there, the slab of plain faces stands in.
	const scene = $("orientation-scene");
	const view = createModelView($("orientation-canvas"), { perspective: PERSPECTIVE });
	let shown = IDENTITY;

	if (view) {
		view.setColours([0.88, 0.89, 0.9], [0.42, 0.44, 0.46]);
		fetch("cato.stl")
			.then((response) => (response.ok
				? response.arrayBuffer()
				: Promise.reject(new Error(`cato.stl: ${response.status}`))))
			.then((buffer) => {
				view.setGeometry(parseStl(buffer));
				view.draw(tilted(cssAxes(shown)));
				scene.classList.add("has-model");
			})
			.catch((err) => console.warn("No 3D model, drawing a slab:", err.message));
	}

	function placeSpots(rotation) {
		const facing = cssFacingViewer(rotation, CAMERA_TILT);

		for (const [axis, [x, y, z]] of Object.entries(SCREEN_SPOTS)) {
			spots[axis].style.transform = `translate3d(${x}px,${y}px,${z}px) ${facing}`;
		}
	}

	function showSpots() {
		for (const [axis, spot] of Object.entries(spots)) {
			spot.hidden = !forward;
			spot.setAttribute("aria-pressed", String(axis === forward));
			spot.textContent = axis === forward ? "screen" : "";
		}
		$("orientation-screen-note").hidden = !forward;
	}

	for (const axis of Object.keys(SCREEN_SPOTS)) {
		const spot = document.createElement("button");
		const side = `${axis[0] === "+" ? "" : "−"}${axis[1].toUpperCase()}`;

		spot.className = "screen-spot";
		spot.type = "button";
		spot.title = `The screen is on the board's ${side} side`;
		spot.setAttribute("aria-label", spot.title);
		spot.addEventListener("click", () => {
			if (axis === forward) {
				return;
			}
			// Moved at once; the app puts it back if the board does not take it.
			forward = axis;
			showSpots();
			setForward(axis);
		});
		model.append(spot);
		spots[axis] = spot;
	}
	placeSpots(IDENTITY);
	showSpots();

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

		const rotation = rotationMatrix(q);

		model.style.transform = cssMatrix(rotation);
		placeSpots(rotation);
		shown = rotation;
		if (view) {
			view.draw(tilted(cssAxes(rotation)));
		}
		$("orientation-roll").textContent = `${roll.toFixed(0)}°`;
		$("orientation-pitch").textContent = `${pitch.toFixed(0)}°`;
		$("orientation-yaw").textContent = `${yaw.toFixed(0)}°`;
	}

	function showNote() {
		note.textContent = !supported
			? "This board's firmware has no orientation stream. Upgrade it to use the viewer."
			: !live
				? "Connect to see the board's orientation."
				: "Heading has no fixed reference: it starts from where the board was " +
				  "and drifts slowly. Face forward sets it back to straight ahead.";
	}

	$("orientation-zero").addEventListener("click", () => {
		zeroYaw = null;
		if (latest && !frame) {
			frame = requestAnimationFrame(draw);
		}
	});

	showNote();

	return {
		/** Call with each orientation the board sends: a quaternion x, y, z, w. */
		sample(q) {
			const now = performance.now();

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
		},
		/**
		 * Call when any of these changes: whether a board with the stream is
		 * connected, whether the connected board's firmware has it, and the
		 * board axis that points at the screen (null when not known).
		 */
		setState(state) {
			if (Boolean(state.live) !== live) {
				latest = null;
				zeroYaw = null;
				count = 0;
				countSince = performance.now();
				$("orientation-rate").textContent = "";
			}
			live = Boolean(state.live);
			supported = state.supported !== false;
			forward = state.forward in SCREEN_SPOTS ? state.forward : null;
			$("orientation-zero").disabled = !live;
			showSpots();
			showNote();
		},
		/** Shows a message in place of the usual note, until the state changes. */
		setNote(text) {
			note.textContent = text;
		},
	};
}

/**
 * The app's side of the viewer: opens its window from the Orientation button
 * in the top row, and has the board stream its orientation while that window
 * is open.
 *
 * @param setForward  async (axis) => void, tells the board which of its axes
 *                    points at the screen
 */
export function initOrientation({ setForward }) {
	const toggle = $("orientation-toggle");
	let popup = null;
	let ready = false; // the window's page has loaded and is listening
	let source = null; // the characteristic, while connected to firmware that has it
	let supported = true;
	let streaming = false;
	let forward = null; // the axis pointing at the screen, once the board has said

	const isOpen = () => Boolean(popup && !popup.closed);
	const tell = (message) => {
		if (isOpen() && ready) {
			popup.postMessage(message, location.origin);
		}
	};
	const tellState = () => tell({
		type: "orientation-state",
		live: Boolean(source),
		supported,
		forward,
	});
	const show = () => toggle.setAttribute("aria-pressed", String(isOpen()));

	function onValue(event) {
		const q = decodeQuaternion(event.target.value);

		if (q) {
			tell({ type: "orientation-sample", q });
		}
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

	// Starts or stops the stream to match: wanted while the window is open.
	function sync() {
		syncing = syncing.then(async () => {
			const target = source;
			const wanted = isOpen() && ready;

			if (!target || streaming === wanted) {
				return;
			}
			try {
				if (wanted) {
					await attempt(() => target.startNotifications());
				} else {
					await attempt(() => target.stopNotifications());
				}
				if (source === target) {
					streaming = wanted;
				}
			} catch (err) {
				if (source === target) {
					tell({
						type: "orientation-note",
						text: `Could not ${wanted ? "start" : "stop"} the stream: ${err.message}`,
					});
				}
			}
		});
		return syncing;
	}

	async function moveScreen(axis) {
		const before = forward;

		if (!(axis in SCREEN_SPOTS) || axis === forward) {
			return;
		}
		forward = axis;
		try {
			await setForward(axis);
		} catch (err) {
			// The window has drawn it moved already: put it back.
			forward = before;
			tellState();
			tell({ type: "orientation-note", text: `Could not move the screen: ${err.message}` });
		}
	}

	// A window closed without saying so (a crash, a kill) is noticed too.
	function checkClosed() {
		if (popup && popup.closed) {
			popup = null;
			ready = false;
			show();
			sync();
		}
	}

	toggle.addEventListener("click", () => {
		if (isOpen()) {
			popup.focus();
			return;
		}
		ready = false;
		// Null if the browser blocks it.
		popup = window.open("orientation.html", "cato-orientation", "popup,width=460,height=600");
		show();
	});

	window.addEventListener("message", (event) => {
		if (event.origin !== location.origin || !popup || event.source !== popup) {
			return;
		}

		const message = event.data || {};

		if (message.type === "orientation-ready") {
			ready = true;
			tellState();
			sync();
		} else if (message.type === "orientation-forward") {
			moveScreen(String(message.axis));
		} else if (message.type === "orientation-closed") {
			// Closed, or reloaded: a reloaded page says it is ready again.
			ready = false;
			sync();
			setTimeout(checkClosed, 200);
		}
	});
	setInterval(checkClosed, 1000);
	window.addEventListener("pagehide", () => {
		if (popup) {
			popup.close();
		}
	});
	show();

	return {
		/**
		 * Call with the board axis that points at the screen, as the board
		 * reports it; null when it is not known (disconnected, old firmware).
		 */
		showForward(axis) {
			forward = axis in SCREEN_SPOTS ? axis : null;
			tellState();
		},
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
			if (!connected) {
				forward = null;
			}
			tellState();
			if (source) {
				source.addEventListener("characteristicvaluechanged", onValue);
				sync();
			}
		},
	};
}
