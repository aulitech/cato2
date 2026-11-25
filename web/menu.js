// SPDX-License-Identifier: MIT
//
// The menu at the end of the top row, and the pages it opens: Settings, Help
// and About. Settings are the app's own (how it looks, what it remembers);
// the board's settings are on the cards.

import { forgetDrives } from "./usb.js";

const STORE_KEY = "cato-settings";
const LAYOUT_KEY = "cato-layout";
const DEFAULT_FONT_SIZE = 13;

const $ = (id) => document.getElementById(id);

export function loadSettings() {
	try {
		return JSON.parse(localStorage.getItem(STORE_KEY)) || {};
	} catch {
		return {};
	}
}

function saveSettings(settings) {
	try {
		localStorage.setItem(STORE_KEY, JSON.stringify(settings));
	} catch {
		// Private windows may refuse: the settings then last until reload.
	}
}

/**
 * @param term   the page's terminal, for the text size setting
 * @param refit  () => void, fits the terminal to its box again
 * @param about  () => [[label, value], ...], the facts About shows, read
 *               each time the page is opened
 */
export function initMenu({ term, refit, about }) {
	const settings = loadSettings();
	const button = $("menu-button");
	const menu = $("menu");

	// ------------------------------------------------------------ Menu ---

	const setOpen = (open) => {
		menu.hidden = !open;
		button.setAttribute("aria-expanded", String(open));
	};

	button.addEventListener("click", () => {
		setOpen(menu.hidden);
		if (!menu.hidden) {
			menu.querySelector("button").focus();
		}
	});
	document.addEventListener("click", (event) => {
		if (!event.target.closest("#menu, #menu-button")) {
			setOpen(false);
		}
	});
	menu.addEventListener("keydown", (event) => {
		const items = [...menu.querySelectorAll("button")];
		const at = items.indexOf(document.activeElement);

		if (event.key === "Escape") {
			setOpen(false);
			button.focus();
		} else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
			event.preventDefault();
			items[(at + (event.key === "ArrowDown" ? 1 : items.length - 1)) % items.length].focus();
		}
	});

	for (const item of menu.querySelectorAll("[data-page]")) {
		item.addEventListener("click", () => {
			const page = $(`page-${item.dataset.page}`);

			setOpen(false);
			if (item.dataset.page === "about") {
				showAbout();
			}
			page.showModal();
		});
	}

	for (const page of document.querySelectorAll("dialog.page")) {
		page.querySelector(".page-close").addEventListener("click", () => page.close());
		// A click on the backdrop, outside the page, closes it.
		page.addEventListener("click", (event) => {
			if (event.target === page) {
				page.close();
			}
		});
	}

	// -------------------------------------------------------- Settings ---

	const theme = $("setting-theme");
	const fontSize = $("setting-font-size");
	const note = $("settings-note");

	function apply() {
		if (settings.theme === "light" || settings.theme === "dark") {
			document.documentElement.dataset.theme = settings.theme;
		} else {
			delete document.documentElement.dataset.theme;
		}
		// The cards look at this class, and are told when it changes.
		document.body.classList.toggle("developer", Boolean(settings.developer));
		window.dispatchEvent(new Event("developer-mode"));
		term.options.fontSize = settings.fontSize || DEFAULT_FONT_SIZE;
		refit();
	}

	const developer = $("setting-developer");

	developer.checked = Boolean(settings.developer);
	developer.addEventListener("change", () => {
		settings.developer = developer.checked;
		saveSettings(settings);
		apply();
	});

	theme.value = settings.theme || "system";
	fontSize.value = String(settings.fontSize || DEFAULT_FONT_SIZE);
	apply();

	theme.addEventListener("change", () => {
		settings.theme = theme.value;
		saveSettings(settings);
		apply();
	});
	fontSize.addEventListener("change", () => {
		settings.fontSize = Number(fontSize.value);
		saveSettings(settings);
		apply();
	});

	$("setting-reset-layout").addEventListener("click", () => {
		try {
			localStorage.removeItem(LAYOUT_KEY);
		} catch {
			// Nothing was remembered, then.
		}
		// The layout is built as the page loads.
		location.reload();
	});
	$("setting-forget-drives").addEventListener("click", async () => {
		await forgetDrives();
		note.textContent = "Forgotten. The next upgrade over USB asks for the drive again.";
	});

	// ----------------------------------------------------------- About ---

	function showAbout() {
		const list = $("about-facts");

		list.replaceChildren();
		for (const [label, value] of about()) {
			const term = document.createElement("dt");
			const detail = document.createElement("dd");

			term.textContent = label;
			detail.textContent = value;
			list.append(term, detail);
		}
	}
}
