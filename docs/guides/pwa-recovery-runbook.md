# Runbook: recovering a Try Turmeric PWA stuck on an old build

An installed Try Turmeric can end up several builds behind and stay there. The
levers a user has -- relaunching the app, the in-app **Force update**, deleting
and re-adding the Home Screen icon -- can all fail to shift it, and on iOS they
have. This is how to reach such a client from the server side.

Read this before deploying the kill-switch. It is two deploys and it takes
offline support away from everyone in between.

## First: is the client actually stale?

Deploy state is checkable from anywhere, and it is worth ruling out before
anything else:

```sh
# What is live, and which build it came from
curl -s https://turmeric-lang.com/sw.js | grep CACHE_VERSION
# -> const CACHE_VERSION = 'tur-try-v1-0.49.2-d32ec4ff7';

# Is a given fix in the deployed CSS?
curl -s https://turmeric-lang.com/try/ | grep -o '/assets/try-[^"]*\.css'
curl -s https://turmeric-lang.com/assets/try-XXXX.css | grep -c -- '--safe-top'
```

If the fix is not in the deployed asset, the client is not the problem: deploy.

To tell what a *device* is running without a cable, the overflow menu shows the
build read from the live service-worker cache name. A device reporting a
different token from the `curl` above is stale.

Do not use "it works in Safari" as evidence. Most of the PWA-specific CSS is
gated on `@media (display-mode: standalone)`, so a Safari tab renders
identically on a stale build and a current one. It discriminates nothing.

## The kill-switch

`web/public/sw-kill.js` is a service worker that, on activation, deletes every
cache on the origin, unregisters itself, and reloads open windows once. It has
no `fetch` handler, so from activation onward every request goes to the network.

It only helps when it is served **at `/sw.js`**. A client refetches the worker
script at the URL it registered and nowhere else; `/sw.js` is served `no-cache`,
so changed bytes there are the one thing that reaches a stuck client without
that client cooperating.

```sh
cd web
TUR_SW_KILL=1 npm run deploy     # arm: /sw.js becomes the kill-switch
# ... let the stuck clients launch once ...
npm run deploy                   # disarm: restore the real worker
```

The armed build prints a loud banner, and `swKillSwitch()` throws rather than
warns if it cannot find a `dist` `sw.js` to replace -- a flag that silently did
nothing would be a recovery everyone believed had happened.

Verify what actually went out before telling anyone it is live:

```sh
curl -s https://turmeric-lang.com/sw.js | head -2
# armed:  // Service worker kill-switch -- recovery for a client stuck ...
# normal: // Try Turmeric service worker.
```

## What a stuck client experiences

One launch while the kill-switch is deployed: the worker installs, every cache
is deleted, the registration is removed, and the page reloads once, arriving
with `?swkill=<token>` in the URL. Everything from that point is served by the
network. The marker is what stops the reload repeating -- see below.

If `client.navigate()` is refused (it is not available in every context), the
wipe and the unregister have still happened and one manual relaunch finishes the
job.

## Hazards

**The reload loop.** While the kill-switch is deployed, `/sw.js` *is* the
kill-switch -- so the page it reloads runs `main.js`, which registers `/sw.js`
on load, which installs the kill-switch again. Without a guard that is an
infinite reload, which is a worse place to leave someone than a stale build. The
`swkill` marker in the URL is that guard: a client already carrying it is not
navigated again. `tests/sw-kill.spec.js` pins this ("reloads the page once, and
does not spin"); it caught the loop before the first deploy.

**Offline support is gone while armed.** No caches, no precached wasm. Anyone
who opens the app on a bad connection during the window gets a broken page.
Keep the window short, and prefer a time when usage is low.

**Disarming is a deploy, not a revert.** Until the second deploy lands, every
visitor keeps re-installing the kill-switch on every load. Harmless, but it
means nobody has offline support until you finish.

## Why the app cannot fix this itself

A fix to the update logic ships *inside* the build a stuck client cannot fetch,
so it can only prevent the next stuck state, never the current one. That is the
whole reason a server-side lever has to exist. See
[docs/reported/pwa-installed-build-cannot-be-updated.md](https://github.com/rjungemann/turmeric/blob/main/docs/reported/pwa-installed-build-cannot-be-updated.md)
for the underlying defect -- `main.js` registers the worker and does nothing
else: no `update()` on resume, no `controllerchange` handler, so an installed
PWA has no way to notice or apply a new build on its own.
