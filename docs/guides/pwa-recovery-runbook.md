# Runbook: recovering a Try Turmeric PWA stuck on an old build

An installed Try Turmeric can end up several builds behind and stay there. The
levers a user has -- relaunching the app, the in-app **Force update**, deleting
and re-adding the Home Screen icon -- can all fail to shift it, and on iOS they
have. This is how to reach such a client from the server side.

Relaunching does now work on a build that carries the update logic described at
the end of this page. A client that predates it does not have that logic and
cannot be sent it, which is the population this runbook is for.

Read this before deploying the kill-switch. It is two deploys and it takes
offline support away from everyone in between.

## First: is the client actually stale?

Deploy state is checkable from anywhere, and it is worth ruling out before
anything else:

```sh
# What is live, and which build it came from
curl -s https://turmeric-lang.com/sw.js | grep CACHE_VERSION
# -> const CACHE_VERSION = 'tur-try-v1-<version>-<commit>';

# Is a given fix in the deployed CSS?
curl -s https://turmeric-lang.com/try/ | grep -o '/assets/try-[^"]*\.css'
curl -s https://turmeric-lang.com/assets/try-XXXX.css | grep -c -- '--safe-top'
```

If the fix is not in the deployed asset, the client is not the problem: deploy.

To tell what a *device* is running without a cable, the overflow menu shows the
build read from the live service-worker cache name. A device reporting a
different token from the `curl` above is stale.

Do not use "it works in Safari" as evidence. The PWA-specific CSS -- the
safe-area insets and the standalone shell both -- is gated on `html.pwa`, which
an inline script in `try/index.html` sets before first paint from
`navigator.standalone` plus the standalone, fullscreen and minimal-ui display
modes. (Not on a bare `@media (display-mode: standalone)`: iOS reports
`fullscreen` for a home-screen app with a black-translucent status bar, so a
standalone-only query is dead on the one platform these rules exist for, and
that is how a black band survived two correct fixes that never ran.) A Safari
tab is not `html.pwa`, so it renders identically on a stale build and a current
one. It discriminates nothing.

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
whole reason a server-side lever has to exist, and it does not stop being the
reason once the update logic is good.

The update logic is now there. `main.js` asks for a new worker on every
foreground and on a persisted `pageshow` -- the browser checks `sw.js` on a
navigation in scope, and a standalone app relaunched from the app switcher is
resumed rather than navigated, so registering on `load` alone meant the check
never ran for the case it was needed in. When a new worker takes over,
a `controllerchange` listener guarded on the page having been controlled at load
re-navigates once; a visible page waits for the next background-then-foreground
so the reload cannot eat an edit still inside its debounces, and an
already-hidden one goes immediately. **Force update** and that re-navigation
both go to a cache-busting URL rather than calling `reload()`, which an iOS
standalone app can answer out of WebKit's own page cache -- Cache Storage is not
the only cache in play.

So the kill-switch is for a client whose installed build is older than that
logic, which is every client that was already stuck when it landed. See
[docs/archive/pwa-installed-build-cannot-be-updated.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/pwa-installed-build-cannot-be-updated.md)
for the original defect and what fixing it turned up that the report had not
predicted.
