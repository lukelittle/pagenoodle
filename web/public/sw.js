/* Pagenoodle service worker.
 * 1. Precaches the entire app (including the PDF engine, OCR model and SQLite) so it works with Wi-Fi off.
 * 2. Refuses every cross-origin request — a second privacy fence behind the Content Security Policy.
 * The precache list and version are injected at build time by scripts/postbuild.mjs.
 */
const VERSION = '__VERSION__';
const PRECACHE = __PRECACHE__;
const CACHE = `pagenoodle-${VERSION}`;

self.addEventListener('install', (event) => {
  event.waitUntil(
    (async () => {
      const cache = await caches.open(CACHE);
      // add in small batches so one slow file does not stall everything
      for (let i = 0; i < PRECACHE.length; i += 20) {
        await cache.addAll(PRECACHE.slice(i, i + 20).map((p) => new Request(p, { cache: 'reload' })));
      }
    })(),
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    (async () => {
      for (const key of await caches.keys()) if (key.startsWith('pagenoodle-') && key !== CACHE) await caches.delete(key);
      await self.clients.claim();
    })(),
  );
});

self.addEventListener('message', (event) => {
  if (event.data === 'skipWaiting') self.skipWaiting();
  if (event.data === 'status') {
    event.source?.postMessage({ type: 'sw-status', version: VERSION, files: PRECACHE.length });
  }
});

self.addEventListener('fetch', (event) => {
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin) {
    // Privacy fence: nothing leaves this device.
    event.respondWith(Response.error());
    return;
  }
  if (event.request.method !== 'GET') return;
  event.respondWith(
    (async () => {
      const cache = await caches.open(CACHE);
      const isNav = event.request.mode === 'navigate';
      const hit = await cache.match(isNav ? './' : event.request, { ignoreSearch: true, ignoreVary: true });
      if (hit) return hit;
      try {
        const res = await fetch(event.request);
        if (res.ok && res.type === 'basic') cache.put(event.request, res.clone());
        return res;
      } catch (e) {
        if (isNav) {
          const shell = await cache.match('./', { ignoreVary: true });
          if (shell) return shell;
        }
        throw e;
      }
    })(),
  );
});
