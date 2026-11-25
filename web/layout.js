// SPDX-License-Identifier: MIT
//
// Page layout: the sections are tabs, which the user puts in order; the console
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

// ------------------------------------------------------------------- Tabs ---

const DRAG_START = 6; // pixels a tab moves before it is being dragged, not clicked

/**
 * The page's sections ("cards" in the markup and the other modules) are laid
 * out as tabs: one shows at a time, chosen from a row of tabs that the user
 * can put in order by dragging. A section marked data-locked (the orientation
 * viewer) is not a tab: it stays in the top left corner, beside the tabs, and
 * folds away by its heading.
 *
 * The section showing has the class "open", which is what the other modules
 * look at to know whether theirs is in view.
 */
function initTabs(layout, persist) {
	const container = $("controls");
	const sections = [...container.querySelectorAll(":scope > .card")];
	const tabbed = document.createElement("div");
	const tablist = document.createElement("div");
	const panels = document.createElement("div");
	const tabs = new Map(); // section name -> its tab

	const setOpen = (card, open) => {
		card.classList.toggle("open", open);
		card.querySelector(".card-head").setAttribute("aria-expanded", String(open));
	};

	tabbed.id = "tabbed";
	tablist.id = "tabs";
	tablist.setAttribute("role", "tablist");
	tablist.setAttribute("aria-label", "Sections");
	panels.id = "panels";
	tabbed.append(tablist, panels);
	container.append(tabbed);

	const remember = () => {
		layout.order = [...tablist.children].map((tab) => tab.dataset.tab);
		persist();
	};

	function select(name, focus = false) {
		for (const [other, tab] of tabs) {
			const selected = other === name;

			tab.setAttribute("aria-selected", String(selected));
			tab.tabIndex = selected ? 0 : -1;
			$(`panel-${other}`).classList.toggle("open", selected);
			if (selected && focus) {
				tab.focus();
			}
		}
		layout.tab = name;
		persist();
	}

	for (const card of sections) {
		const name = card.dataset.card;
		const head = card.querySelector(".card-head");

		if ("locked" in card.dataset) {
			// Not a tab: folds by its heading, closed to begin with.
			const toggle = () => setOpen(card, !card.classList.contains("open"));

			setOpen(card, false);
			head.setAttribute("role", "button");
			head.setAttribute("tabindex", "0");
			head.addEventListener("click", (event) => {
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
			continue;
		}

		const tab = document.createElement("button");

		tab.type = "button";
		tab.id = `tab-${name}`;
		tab.dataset.tab = name;
		tab.textContent = card.querySelector("h2").textContent;
		tab.setAttribute("role", "tab");
		tab.setAttribute("aria-controls", `panel-${name}`);
		tab.title = "Drag to move this tab";
		card.id = `panel-${name}`;
		card.setAttribute("role", "tabpanel");
		card.setAttribute("aria-labelledby", tab.id);
		panels.append(card);
		tabs.set(name, tab);

		tab.addEventListener("keydown", (event) => {
			const all = [...tablist.children];
			const at = all.indexOf(tab);
			const to = {
				ArrowLeft: at - 1,
				ArrowRight: at + 1,
				Home: 0,
				End: all.length - 1,
			}[event.key];

			if (to === undefined) {
				return;
			}
			event.preventDefault();
			select(all[(to + all.length) % all.length].dataset.tab, true);
		});

		// A press that moves is a drag: the tab trades places with whichever
		// tab the pointer is over. One that does not is a click, and selects.
		let start = null;
		let dragging = false;
		let passed = null; // the tab last traded with, until the pointer leaves it

		const onMove = (event) => {
			if (!dragging) {
				if (Math.hypot(event.clientX - start.x, event.clientY - start.y) < DRAG_START) {
					return;
				}
				dragging = true;
				tab.classList.add("dragging");
			}

			const over = document.elementFromPoint(event.clientX, event.clientY)
				?.closest("#tabs > [role=tab]");

			if (!over || over === tab) {
				passed = null;
				return;
			}
			if (over === passed) {
				return;
			}

			const all = [...tablist.children];

			over[all.indexOf(over) > all.indexOf(tab) ? "after" : "before"](tab);
			passed = over;
		};

		// Listened for on the window: moving the tab in the page would end a
		// pointer capture it held.
		const onUp = () => {
			window.removeEventListener("pointermove", onMove);
			window.removeEventListener("pointerup", onUp);
			window.removeEventListener("pointercancel", onUp);
			tab.classList.remove("dragging");
			if (dragging) {
				remember();
			} else {
				select(name);
			}
			dragging = false;
		};

		tab.addEventListener("pointerdown", (event) => {
			if (event.button !== 0) {
				return;
			}
			start = { x: event.clientX, y: event.clientY };
			passed = null;
			window.addEventListener("pointermove", onMove);
			window.addEventListener("pointerup", onUp);
			window.addEventListener("pointercancel", onUp);
		});
		// From the keyboard, and from anything else that clicks without a pointer.
		tab.addEventListener("click", (event) => {
			if (event.detail === 0) {
				select(name);
			}
		});
	}

	// Remembered order first; tabs added since then follow in their own.
	const order = (layout.order || []).filter((name) => tabs.has(name));

	for (const name of [...order, ...[...tabs.keys()].filter((name) => !order.includes(name))]) {
		tablist.append(tabs.get(name));
	}
	delete layout.folded; // from when sections were cards that folded

	select(tabs.has(layout.tab) ? layout.tab : tablist.firstElementChild.dataset.tab);

	// News on a tab that is not showing would go unseen: a firmware upgrade on
	// offer puts a dot on its tab.
	const badge = $("firmware-badge");

	new MutationObserver(() => {
		tabs.get("firmware").classList.toggle("flagged", /available|found/i.test(badge.textContent));
	}).observe(badge, { childList: true, characterData: true, subtree: true });
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

	initTabs(layout, persist);

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
