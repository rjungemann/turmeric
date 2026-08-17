// Try Turmeric service worker.
//
// Strategy:
//   - Precache the PWA shell (/try/) and the heavy WASM/JS assets on install.
//   - Runtime: cache-first for same-origin static assets (with background
//     revalidation), network-first for HTML navigations and /docs/*.
//
// CACHE_VERSION must change on every release so `activate` evicts the old
// caches AND the changed sw.js bytes make the browser re-install the worker --
// otherwise a cache-first asset (turmeric.js / turmeric.wasm) is served from a
// stale precache forever, no matter how much newer the deploy is.
//
// The version token below is rewritten to the real VERSION at build time by the
// `injectSwVersion` plugin in vite.config.js (it regex-replaces the
// `tur-try-v1-<x.y.z>` token in dist/sw.js). The literal here is the dev/no-build
// fallback; keep it in sync with VERSION so an un-built serve is still correct.
const CACHE_VERSION = 'tur-try-v1-0.34.0';
const PRECACHE = `${CACHE_VERSION}-precache`;
const RUNTIME  = `${CACHE_VERSION}-runtime`;

const PRECACHE_URLS = [
    '/try/',
    '/main.js',
    '/styles.css',
    '/site.css',
    '/site.js',
    '/turmeric.js',
    '/turmeric.wasm',
    '/doc-names.json',
    '/favicon.svg',
    '/manifest.webmanifest',
    '/icons/icon-192.png',
    '/icons/icon-512.png',
    '/icons/apple-touch-icon.png',
];

self.addEventListener('install', (event) => {
    event.waitUntil((async () => {
        const cache = await caches.open(PRECACHE);
        // addAll is atomic -- if any URL 404s the whole install fails. Add
        // them individually so a missing optional file doesn't brick the SW.
        await Promise.all(PRECACHE_URLS.map(async (url) => {
            try {
                const res = await fetch(url, { cache: 'reload' });
                if (res && res.ok) await cache.put(url, res.clone());
            } catch (_) { /* offline-only first load -- skip */ }
        }));
        await self.skipWaiting();
    })());
});

self.addEventListener('activate', (event) => {
    event.waitUntil((async () => {
        const keys = await caches.keys();
        await Promise.all(keys
            .filter((k) => k !== PRECACHE && k !== RUNTIME)
            .map((k) => caches.delete(k)));
        await self.clients.claim();
    })());
});

self.addEventListener('message', (event) => {
    if (event.data === 'SKIP_WAITING') self.skipWaiting();
});

const isHtmlNavigation = (request) =>
    request.mode === 'navigate' ||
    (request.method === 'GET' &&
     request.headers.get('accept') &&
     request.headers.get('accept').includes('text/html'));

self.addEventListener('fetch', (event) => {
    const request = event.request;
    if (request.method !== 'GET') return;

    const url = new URL(request.url);
    if (url.origin !== self.location.origin) return;

    // Network-first for HTML navigations and /docs/* so doc pages stay fresh.
    if (isHtmlNavigation(request) || url.pathname.startsWith('/docs/')) {
        event.respondWith(networkFirst(request));
        return;
    }

    // Cache-first for everything else (assets, wasm).
    event.respondWith(cacheFirst(request));
});

async function networkFirst(request) {
    const cache = await caches.open(RUNTIME);
    try {
        const fresh = await fetch(request);
        if (fresh && fresh.ok) cache.put(request, fresh.clone());
        return fresh;
    } catch (e) {
        const cached = await cache.match(request) ||
                       await caches.match(request) ||
                       await caches.match('/try/');
        if (cached) return cached;
        throw e;
    }
}

async function cacheFirst(request) {
    const cached = await caches.match(request);
    if (cached) {
        // Background revalidate; ignore failure (offline).
        fetch(request).then(async (res) => {
            if (res && res.ok) {
                const cache = await caches.open(RUNTIME);
                cache.put(request, res.clone());
            }
        }).catch(() => {});
        return cached;
    }
    try {
        const fresh = await fetch(request);
        if (fresh && fresh.ok) {
            const cache = await caches.open(RUNTIME);
            cache.put(request, fresh.clone());
        }
        return fresh;
    } catch (e) {
        const fallback = await caches.match('/try/');
        if (fallback && isHtmlNavigation(request)) return fallback;
        throw e;
    }
}
