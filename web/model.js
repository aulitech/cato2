// SPDX-License-Identifier: MIT
//
// Draws the Cato's case, from its STL model, for the orientation viewer.
//
// The viewer's screen and dots are HTML elements turned with CSS 3D
// transforms, so the model is drawn with the very same projection: positions
// are in CSS pixels about the centre of the scene, X right, Y down, Z towards
// the viewer, seen from `perspective` pixels away. The two then line up
// without either knowing about the other.

// How the model's own axes lie on the board's (the IMU's). In the model the
// USB socket points along -X and the LED side, which carries the logo, along
// +Z. On the board the USB socket points along -X too (measured: with the USB
// end up, the accelerometer reads -1 g on X), and the LED side is its top,
// +Z. So the two sets of axes are the same.
const BOARD_FROM_MODEL = ([x, y, z]) => [x, y, z];

const PIXELS_PER_MM = 5;

/** Reads a binary STL into positions and normals, in board axes and pixels. */
export function parseStl(buffer) {
	const view = new DataView(buffer);
	const count = view.getUint32(80, true);

	if (buffer.byteLength < 84 + 50 * count) {
		throw new Error("Not a binary STL file");
	}

	const positions = new Float32Array(count * 9);
	const normals = new Float32Array(count * 9);
	const low = [Infinity, Infinity, Infinity];
	const high = [-Infinity, -Infinity, -Infinity];
	const vector = (offset) => BOARD_FROM_MODEL(
		[0, 4, 8].map((at) => view.getFloat32(offset + at, true)));

	for (let i = 0; i < count; i++) {
		const facet = 84 + 50 * i;
		const corners = [12, 24, 36].map((at) => vector(facet + at));
		const [a, b, c] = corners;

		// The file's own normals are not always filled in: work them out.
		const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
		const v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
		const n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
		const length = Math.hypot(...n) || 1;

		corners.forEach((corner, k) => {
			positions.set(corner, i * 9 + k * 3);
			normals.set(n.map((value) => value / length), i * 9 + k * 3);
			corner.forEach((value, axis) => {
				low[axis] = Math.min(low[axis], value);
				high[axis] = Math.max(high[axis], value);
			});
		});
	}

	// About its own middle, and in pixels.
	for (let i = 0; i < positions.length; i++) {
		const axis = i % 3;

		positions[i] = (positions[i] - (low[axis] + high[axis]) / 2) * PIXELS_PER_MM;
	}

	return { positions, normals, count: count * 3 };
}

const VERTEX_SHADER = `
attribute vec3 aPosition;
attribute vec3 aNormal;
uniform mat3 uTurn;        // board axes to the scene's CSS axes
uniform vec2 uHalf;        // half the scene's size, in CSS pixels
uniform float uPerspective;
varying vec3 vNormal;

void main() {
	vec3 p = uTurn * aPosition;
	float w = 1.0 - p.z / uPerspective;

	vNormal = uTurn * aNormal;
	// Nearer the viewer is a larger CSS Z and has to be a smaller depth.
	gl_Position = vec4(p.x / uHalf.x, -p.y / uHalf.y, -p.z / 1000.0 * w, w);
}`;

const FRAGMENT_SHADER = `
precision mediump float;
uniform vec3 uLight;
uniform vec3 uDark;
varying vec3 vNormal;

void main() {
	// Lit from the upper left, in front (CSS axes: Y is down).
	float t = 0.5 * (1.0 + dot(normalize(vNormal), normalize(vec3(-0.5, -1.0, 0.8))));

	gl_FragColor = vec4(mix(uDark, uLight, t), 1.0);
}`;

/**
 * Sets up drawing on a canvas that covers the scene. Returns null if the
 * browser has no WebGL.
 *
 * @param perspective  the scene's CSS perspective, in pixels
 */
export function createModelView(canvas, { perspective }) {
	const gl = canvas.getContext("webgl", { antialias: true, alpha: true });

	if (!gl) {
		return null;
	}

	const program = gl.createProgram();

	for (const [type, source] of [[gl.VERTEX_SHADER, VERTEX_SHADER],
				      [gl.FRAGMENT_SHADER, FRAGMENT_SHADER]]) {
		const shader = gl.createShader(type);

		gl.shaderSource(shader, source);
		gl.compileShader(shader);
		gl.attachShader(program, shader);
	}
	gl.linkProgram(program);
	if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
		return null;
	}
	gl.useProgram(program);
	gl.enable(gl.DEPTH_TEST);

	const uniform = (name) => gl.getUniformLocation(program, name);
	let vertices = 0;
	let turn = [1, 0, 0, 0, 1, 0, 0, 0, 1];

	function attribute(name, data) {
		const location = gl.getAttribLocation(program, name);

		gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
		gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
		gl.enableVertexAttribArray(location);
		gl.vertexAttribPointer(location, 3, gl.FLOAT, false, 0, 0);
	}

	function draw() {
		const width = canvas.clientWidth;
		const height = canvas.clientHeight;
		const ratio = window.devicePixelRatio || 1;

		if (!width || !height) {
			return;
		}
		if (canvas.width !== Math.round(width * ratio) ||
		    canvas.height !== Math.round(height * ratio)) {
			canvas.width = Math.round(width * ratio);
			canvas.height = Math.round(height * ratio);
		}
		gl.viewport(0, 0, canvas.width, canvas.height);
		gl.clearColor(0, 0, 0, 0);
		gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
		if (!vertices) {
			return;
		}
		gl.uniformMatrix3fv(uniform("uTurn"), false, turn);
		gl.uniform2f(uniform("uHalf"), width / 2, height / 2);
		gl.uniform1f(uniform("uPerspective"), perspective);
		gl.drawArrays(gl.TRIANGLES, 0, vertices);
	}

	new ResizeObserver(draw).observe(canvas);

	return {
		setGeometry({ positions, normals, count }) {
			attribute("aPosition", positions);
			attribute("aNormal", normals);
			vertices = count;
			draw();
		},
		/** Shades between two colours, each [r, g, b] from 0 to 1. */
		setColours(light, dark) {
			gl.uniform3fv(uniform("uLight"), light);
			gl.uniform3fv(uniform("uDark"), dark);
			draw();
		},
		/**
		 * Draws the model turned by a 3x3 matrix (rows) from board axes to
		 * the scene's CSS axes.
		 */
		draw(matrix) {
			// Column by column, as WebGL wants it.
			turn = [0, 1, 2].flatMap((column) => matrix.map((row) => row[column]));
			draw();
		},
	};
}
