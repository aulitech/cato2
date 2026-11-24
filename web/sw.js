// SPDX-License-Identifier: MIT
//
// Service worker: makes the app installable and usable offline. Requests go
// to the network first, so an edited or redeployed app shows up on the next
// load; the cached copy is used only when the server cannot be reached.

const CACHE = "xiao-sense-plus-v2";

const FILES = [
	"./",
	"index.html",
	"app.js",
	"controls.js",
	"shell.js",
	"smp.js",
	"manifest.webmanifest",
	"vendor/xterm.js",
	"vendor/xterm.css",
	"vendor/addon-fit.js",
	"icons/icon.svg",
	"icons/icon-192.png",
	"icons/icon-512.png",
	"icons/maskable-512.png",
	"icons/apple-touch-icon.png",
];

self.addEventListener("install", (event) => {
	event.waitUntil(
		caches.open(CACHE)
			.then((cache) => cache.addAll(FILES))
			.then(() => self.skipWaiting())
	);
});

self.addEventListener("activate", (event) => {
	event.waitUntil(
		caches.keys()
			.then((names) => Promise.all(
				names.filter((name) => name !== CACHE).map((name) => caches.delete(name))
			))
			.then(() => self.clients.claim())
	);
});

self.addEventListener("fetch", (event) => {
	const request = event.request;

	if (request.method !== "GET" || new URL(request.url).origin !== self.location.origin) {
		return;
	}

	event.respondWith(
		fetch(request)
			.then((response) => {
				if (response.ok) {
					const copy = response.clone();

					caches.open(CACHE).then((cache) => cache.put(request, copy));
				}
				return response;
			})
			.catch(async () => {
				const cached = await caches.match(request, { ignoreSearch: true });

				return cached || Response.error();
			})
	);
});
