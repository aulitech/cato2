// SPDX-License-Identifier: MIT
//
// The app's own confirmation dialog, in place of the browser's confirm():
// it looks like the rest of the app, names the action on its button, and
// does not stop the page (and with it the connection) while it is open.

const $ = (id) => document.getElementById(id);

/**
 * Asks the user to confirm. The message is a question, optionally followed
 * by a blank line and what the user should know before answering.
 *
 * @param action  the label of the confirming button
 * @param danger  true if the action destroys something
 * @returns a promise of true if confirmed, false if cancelled or dismissed
 */
export function ask(message, { action = "OK", danger = false } = {}) {
	const dialog = $("ask");
	const confirm = $("ask-confirm");
	const [question, ...details] = message.split("\n\n");

	$("ask-question").textContent = question;
	$("ask-details").replaceChildren(...details.map((text) => {
		const paragraph = document.createElement("p");

		paragraph.textContent = text;
		return paragraph;
	}));
	confirm.textContent = action;
	confirm.className = danger ? "danger" : "primary";

	return new Promise((resolve) => {
		const answer = (event) => {
			if (event.type === "click" && !event.target.closest("button")) {
				const box = dialog.getBoundingClientRect();

				// Only a click outside the box, on the backdrop, is a Cancel.
				if (event.clientX >= box.left && event.clientX <= box.right &&
				    event.clientY >= box.top && event.clientY <= box.bottom) {
					return;
				}
			}
			dialog.removeEventListener("click", answer);
			dialog.removeEventListener("close", answer);
			if (dialog.open) {
				dialog.close();
			}
			resolve(event.type === "click" && event.target === confirm);
		};

		dialog.addEventListener("click", answer);
		dialog.addEventListener("close", answer); // Escape
		dialog.showModal();
		// Enter must not confirm something destructive by accident.
		(danger ? $("ask-cancel") : confirm).focus();
	});
}
