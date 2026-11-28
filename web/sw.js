// SPDX-License-Identifier: MIT
//
// Service worker: makes the app installable and usable offline. Requests go
// to the network first, so an edited or redeployed app shows up on the next
// load; the cached copy is used only when the server cannot be reached.

importScripts("version.js");

// A new version of the app gets a new cache, and the old one is dropped.
const CACHE = `mycato-${self.MYCATO_VERSION}`;

const FILES = [
	"./",
	"index.html",
	"app.js",
	"console.html",
	"controls.js",
	"dialog.js",
	"layout.js",
	"menu.js",
	"model.js",
	"cato.stl",
	"orientation.html",
	"orientation.js",
	"shell.js",
	"smp.js",
	"usb.js",
	"version.js",
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
