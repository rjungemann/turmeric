// Service worker kill-switch -- recovery for a client stuck on a build it will
// not let go of.
//
// DEPLOYED AT /sw.js, not alongside it. The browser only refetches a worker
// script at the URL it registered, and /sw.js is served `no-cache`, so changed
// bytes there are the one thing that reaches a stuck client without the client
// cooperating. Build it with TUR_SW_KILL=1 (see swKillSwitch() in
// vite.config.js), deploy, let the stuck client launch once, then deploy again
// WITHOUT the flag to restore the real worker.
//
// Why this exists: an installed Try Turmeric on iOS sat several builds behind
// for months, immune to relaunching, to the in-app "Force update", and to
// deleting and re-adding the icon. See
// docs/reported/pwa-installed-build-cannot-be-updated.md.
//
// The previous version of this file was never run, never tested, and referenced
// by nothing. It did its work in an `async` listener with no event.waitUntil(),
// so the browser was free to terminate the worker at the first await -- i.e.
// before a single cache was deleted. Insurance nobody had checked.

// Query parameter stamped on a client this worker has already reloaded. See
// the navigation step in `activate` for why re-entry has to be stopped.
const KILL_MARKER = 'swkill';

self.addEventListener('install', (event) => {
    // Do not wait for the old worker's clients to close. Reaching a client that
    // is never closed is the entire job.
    event.waitUntil(self.skipWaiting());
});

self.addEventListener('activate', (event) => {
    // event.waitUntil is load-bearing, not decoration: everything below is
    // after an await, and without it the worker can be killed mid-wipe.
    event.waitUntil((async () => {
        // Claim first -- the windows this navigates at the end have to be ours.
        await self.clients.claim();

        // Every cache on the origin, by name, whatever it is called. A stuck
        // client's cache names come from the build it is stuck on, so nothing
        // here may assume today's CACHE_VERSION scheme.
        const keys = await caches.keys();
        await Promise.all(keys.map((k) => caches.delete(k).catch(() => false)));

        // Unregister BEFORE navigating. Navigating can be the last thing a
        // client does with this worker, and if that tears the worker down the
        // registration must already be gone -- that is the outcome that
        // actually matters. The navigation below is a convenience on top.
        await self.registration.unregister();

        // Re-fetch what is on screen. Without this a stuck PWA keeps rendering
        // the assets it already has in memory, and the reader has to relaunch
        // to see any difference -- which is the move that was already not
        // working for them.
        //
        // A cache-busting parameter, not client.url verbatim: WebKit's own
        // HTTP/page cache is not Cache Storage and was not touched above, and
        // it will happily answer a same-URL navigation with the very HTML this
        // is trying to replace. The query does not affect scope matching, so
        // the app stays a PWA, and the next cold launch uses the manifest's
        // start_url and drops it.
        //
        // NAVIGATE AT MOST ONCE PER CLIENT, and that guard is not a nicety.
        // While this file is deployed, /sw.js IS the kill-switch -- so the page
        // this navigates runs main.js, which registers /sw.js on load, which
        // installs the kill-switch again, which wipes, unregisters and
        // navigates again. A PWA spinning in a reload loop is a worse place to
        // leave someone than a stale build. The marker already in the URL is
        // what says "this client has been through here"; seeing it, stop.
        const windows = await self.clients.matchAll({
            type: 'window',
            includeUncontrolled: true,
        });
        await Promise.all(windows.map(async (client) => {
            try {
                const url = new URL(client.url);
                if (url.searchParams.has(KILL_MARKER)) return;
                url.searchParams.set(KILL_MARKER, Date.now().toString(36));
                await client.navigate(url.href);
            } catch (_) {
                // navigate() is refused in some contexts. The unregister and
                // the cache wipe already happened; one relaunch finishes it.
            }
        }));
    })());
});

// No `fetch` handler, deliberately. A worker with none is bypassed entirely for
// subresource and navigation requests, so from activation onward every request
// goes to the network even in the window between activate and unregister
// taking effect.
