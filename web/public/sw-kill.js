// Service worker kill-switch.
//
// If a buggy service worker is ever shipped, replacing /sw.js with this
// file (or pointing the kill route at this script) will unregister all
// service workers under this origin and drop every cache.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', async () => {
    self.clients.claim();
    const keys = await caches.keys();
    await Promise.all(keys.map((k) => caches.delete(k)));
    const regs = await self.registration.unregister();
    return regs;
});
