// sw.js - caches the app shell so the installed app opens without a network connection.
// Vite gives the JS/CSS bundles hashed names, so only the fixed-name files are precached here;
// the bundles are cached by the fetch handler the first time the page loads them.
const VERSION = 'emu8sd-v2';
const SHELL = ['./', 'index.html', 'icon.svg', 'manifest.webmanifest'];

self.addEventListener('install', ev => {
  ev.waitUntil(caches.open(VERSION).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', ev => {
  ev.waitUntil(caches.keys()
    .then(keys => Promise.all(keys.filter(k => k !== VERSION).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});

// Network first (so edits show up while developing), cache when offline.
self.addEventListener('fetch', ev => {
  if (ev.request.method !== 'GET' || new URL(ev.request.url).origin !== location.origin) return;
  ev.respondWith(fetch(ev.request)
    .then(res => {
      if (res.ok) { const copy = res.clone(); caches.open(VERSION).then(c => c.put(ev.request, copy)); }
      return res;
    })
    .catch(() => caches.match(ev.request)));
});
