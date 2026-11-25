// SPDX-License-Identifier: MIT
//
// Page layout: the cards are the page, and the user arranges them; the console
// lives in a drawer that docks to the left, right or bottom of the window, or
// pops out into a window of its own. Everything chosen is remembered.

const STORE_KEY = "cato-layout";
const DOCKS = ["left", "bottom", "right"];
const POPOUT = "popout";
const HISTORY_BYTES = 256 * 1024; // console output replayed into a new window
const MIN_SIZE = 160;

const $ = (id) => document.getElementById(id);

function load() {
	try {
		return JSON.parse(localStorage.getItem(STORE_KEY)) || {};
	} catch {
		return {};
	}
}

function save(layout) {
	try {
		localStorage.setItem(STORE_KEY, JSON.stringify(layout));
	} catch {
		// Private windows may refuse: the layout then lasts until reload.
	}
}

// ------------------------------------------------------------------ Cards ---

/**
 * Cards start folded and open one at a time; a card marked data-independent
 * (the orientation viewer) opens and closes on its own, beside whichever
 * other is open. They are reordered by dragging the grip in their heading
 * (or with the arrow keys while the grip has focus).
 */
function initCards(layout, persist) {
	const container = $("controls");
	const cards = () => [...container.querySelectorAll(":scope > .card")];
	const independent = (card) => "independent" in card.dataset;

	const remember = () => {
		layout.order = cards().map((card) => card.dataset.card);
		persist();
	};

	const setOpen = (card, open) => {
		card.classList.toggle("open", open);
		card.querySelector(".card-head").setAttribute("aria-expanded", String(open));
	};

	const toggle = (card) => {
		const open = !card.classList.contains("open");

		if (open && !independent(card)) {
			for (const other of cards()) {
				if (other !== card && !independent(other)) {
					setOpen(other, false);
				}
			}
		}
		setOpen(card, open);
	};

	delete layout.folded; // from when open cards were remembered

	// Remembered order first; cards added since keep their place at the end.
	for (const name of layout.order || []) {
		const card = container.querySelector(`:scope > [data-card="${name}"]`);

		if (card) {
			container.append(card);
		}
	}
	for (const card of cards()) {
		if (!(layout.order || []).includes(card.dataset.card) && layout.order) {
			container.append(card);
		}
	}

	for (const card of cards()) {
		const head = card.querySelector(".card-head");
		const title = card.querySelector("h2").textContent;
		const grip = document.createElement("button");

		setOpen(card, false);

		head.setAttribute("role", "button");
		head.setAttribute("tabindex", "0");
		head.addEventListener("click", (event) => {
			// Buttons in the heading act; they do not fold the card.
			if (!event.target.closest("button")) {
				toggle(card);
			}
		});
		head.addEventListener("keydown", (event) => {
			if (event.target === head && (event.key === "Enter" || event.key === " ")) {
				event.preventDefault();
				toggle(card);
			}
		});

		grip.className = "card-grip";
		grip.type = "button";
		grip.title = "Drag to move this card";
		grip.setAttribute("aria-label", `Move the ${title} card: drag, or use the arrow keys`);
		head.prepend(grip);

		grip.addEventListener("keydown", (event) => {
			const earlier = event.key === "ArrowUp" || event.key === "ArrowLeft";
			const later = event.key === "ArrowDown" || event.key === "ArrowRight";

			if (!earlier && !later) {
				return;
			}
			event.preventDefault();

			const other = earlier ? card.previousElementSibling : card.nextElementSibling;

			if (other) {
				other[earlier ? "before" : "after"](card);
				grip.focus();
				remember();
			}
		});

		// The card stays in the page while dragged and trades places with
		// whichever card the pointer is over.
		let passed = null; // the card last traded with, until the pointer leaves it

		const onMove = (event) => {
			const over = document.elementFromPoint(event.clientX, event.clientY)
				?.closest("#controls > .card");
			const box = container.getBoundingClientRect();

			if (event.clientY < box.top + 40) {
				container.scrollBy(0, -12);
			} else if (event.clientY > box.bottom - 40) {
				container.scrollBy(0, 12);
			}
			if (!over || over === card) {
				passed = null;
				return;
			}
			if (over === passed) {
				return;
			}

			const all = cards();

			over[all.indexOf(over) > all.indexOf(card) ? "after" : "before"](card);
			passed = over;
		};

		// Listened for on the window: moving the card in the page would end
		// a pointer capture held by its grip.
		const drop = () => {
			card.classList.remove("dragging");
			window.removeEventListener("pointermove", onMove);
			window.removeEventListener("pointerup", drop);
			window.removeEventListener("pointercancel", drop);
			remember();
		};

		grip.addEventListener("pointerdown", (event) => {
			if (event.button !== 0) {
				return;
			}
			event.preventDefault();
			card.classList.add("dragging");
			passed = null;
			window.addEventListener("pointermove", onMove);
			window.addEventListener("pointerup", drop);
			window.addEventListener("pointercancel", drop);
		});
	}
}

// ---------------------------------------------------------------- Console ---

/**
 * @param term       the page's terminal
 * @param sendInput  (string) => void, what is typed in the popped-out console
 * @returns { output(bytes|string), clear() } to be told of everything shown
 */
export function initLayout({ term, sendInput }) {
	const layout = load();
	const persist = () => save(layout);
	const body = document.body;
	const drawer = $("console");
	const resizer = $("console-resizer");
	const toggle = $("console-toggle");
	const encoder = new TextEncoder();
	let history = [];
	let historyBytes = 0;
	let popup = null;
	let popupReady = false;

	if (!DOCKS.includes(layout.dock)) {
		layout.dock = "right";
	}
	layout.open = layout.open !== false;

	initCards(layout, persist);

	const poppedOut = () => Boolean(popup && !popup.closed);

	function show() {
		const out = poppedOut();

		body.dataset.dock = layout.dock;
		body.dataset.console = out ? "popout" : layout.open ? "open" : "closed";
		drawer.style.setProperty("--console-side", `${layout.side || 480}px`);
		drawer.style.setProperty("--console-bottom", `${layout.bottom || 300}px`);
		toggle.setAttribute("aria-pressed", String(out || layout.open));
		toggle.textContent = out ? "Console (in its window)" : "Console";
		for (const button of drawer.querySelectorAll("[data-dock]")) {
			button.setAttribute("aria-pressed", String(button.dataset.dock === layout.dock));
		}
	}

	function closePopup() {
		if (popup) {
			popup.close();
		}
		popup = null;
		popupReady = false;
	}

	function dock(where) {
		closePopup();
		layout.dock = where;
		layout.open = true;
		persist();
		show();
		term.focus();
	}

	function popOut() {
		if (poppedOut()) {
			popup.focus();
			return;
		}
		popupReady = false;
		popup = window.open("console.html", "cato-console", "popup,width=760,height=480");
		if (!popup) {
			// Blocked by the browser: the drawer stays.
			return;
		}
		show();
	}

	for (const button of drawer.querySelectorAll("[data-dock]")) {
		button.addEventListener("click", () => {
			if (button.dataset.dock === POPOUT) {
				popOut();
			} else {
				dock(button.dataset.dock);
			}
		});
	}

	$("console-close").addEventListener("click", () => {
		layout.open = false;
		persist();
		show();
	});

	toggle.addEventListener("click", () => {
		if (poppedOut()) {
			popup.focus();
			return;
		}
		layout.open = !layout.open;
		persist();
		show();
		if (layout.open) {
			term.focus();
		}
	});

	// The popped-out console is a page of its own, fed over messages.
	window.addEventListener("message", (event) => {
		if (event.origin !== location.origin || !popup || event.source !== popup) {
			return;
		}

		const message = event.data || {};

		if (message.type === "console-ready") {
			popupReady = true;
			for (const chunk of history) {
				popup.postMessage({ type: "console-output", bytes: chunk }, location.origin);
			}
		} else if (message.type === "console-input") {
			sendInput(String(message.data));
		} else if (message.type === "console-clear") {
			clear(false);
		} else if (message.type === "console-dock" || message.type === "console-closed") {
			popup = null;
			popupReady = false;
			layout.open = true;
			show();
		}
	});

	// A window closed without saying so (a crash, a kill) is noticed too.
	setInterval(() => {
		if (popup && popup.closed) {
			popup = null;
			popupReady = false;
			show();
		}
	}, 1000);
	window.addEventListener("pagehide", closePopup);

	// Dragging the bar between cards and console resizes the drawer.
	const sideways = () => !getComputedStyle($("workspace")).flexDirection.startsWith("column");

	function resizeTo(event) {
		const box = $("workspace").getBoundingClientRect();
		let size;

		if (!sideways()) {
			size = box.bottom - event.clientY;
			layout.bottom = Math.round(Math.max(MIN_SIZE, Math.min(size, box.height - MIN_SIZE)));
		} else {
			size = layout.dock === "left" ? event.clientX - box.left : box.right - event.clientX;
			layout.side = Math.round(Math.max(MIN_SIZE, Math.min(size, box.width - MIN_SIZE)));
		}
		show();
	}

	resizer.addEventListener("pointerdown", (event) => {
		event.preventDefault();
		resizer.setPointerCapture(event.pointerId);
		resizer.addEventListener("pointermove", resizeTo);
	});

	const endResize = () => {
		resizer.removeEventListener("pointermove", resizeTo);
		persist();
	};

	resizer.addEventListener("pointerup", endResize);
	resizer.addEventListener("pointercancel", endResize);
	resizer.addEventListener("keydown", (event) => {
		const step = { ArrowLeft: -24, ArrowUp: 24, ArrowRight: 24, ArrowDown: -24 }[event.key];

		if (!step) {
			return;
		}
		event.preventDefault();
		if (!sideways()) {
			layout.bottom = Math.max(MIN_SIZE, (layout.bottom || 300) + step);
		} else {
			// Towards the cards makes the drawer bigger.
			const grow = (layout.dock === "left") === (event.key === "ArrowRight" ||
								    event.key === "ArrowDown");

			layout.side = Math.max(MIN_SIZE, (layout.side || 480) + (grow ? 24 : -24));
		}
		persist();
		show();
	});

	function clear(tellPopup = true) {
		history = [];
		historyBytes = 0;
		term.clear();
		if (tellPopup && poppedOut() && popupReady) {
			popup.postMessage({ type: "console-clear" }, location.origin);
		}
	}

	show();

	return {
		/** Everything written to the terminal, for the popped-out copy. */
		output(data) {
			const bytes = typeof data === "string" ? encoder.encode(data) : data.slice();

			history.push(bytes);
			historyBytes += bytes.length;
			while (historyBytes > HISTORY_BYTES && history.length > 1) {
				historyBytes -= history.shift().length;
			}
			if (poppedOut() && popupReady) {
				popup.postMessage({ type: "console-output", bytes }, location.origin);
			}
		},
		clear,
	};
}
