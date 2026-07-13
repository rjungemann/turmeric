# Plan: Leak-Detection Follow-ups -- the `requires.no-leak-check` surface

> **Status:** P1 landed (reactor callback-box ownership; 9 fixtures unmarked).
> Fiber-body ownership landed (5 `reactor-fibers-*` unmarked). Remaining
> P1-next: httpd handler-chain drop.
> **Last Updated:** 2026-07-13
> **Type:** Build/test hygiene + runtime memory-management
> **Related:**
> - `docs/archive/history/asan-debug-leaks-plan.md` -- the *predecessor* plan
>   that made the **compiler** (`tur build`/`emit-c`) leak-clean and turned
>   `bash tests/run.sh` into a real net. This plan is the follow-up for the
>   **spawned program** leak surface it did not cover.
> - `docs/reported/cps-delimited-dk-node-leak.md` -- the open DK-node report.
> - `tests/run.sh` -- `requires.no-leak-check` handling (hook path ~357-361,
>   compiled path ~437-445).
> - `src/main.c` -- `autolink_needs_asan` detection (~1980-2011) and the
>   `-fsanitize=address,undefined` propagation to the built program (~2080).
> - `src/async/reactor.c`, `src/async/reactor.h` -- reactor callback-box
>   ownership (`owns_cb`, `tur_reactor_disown_cb`); `stdlib/httpd.tur` -- the
>   httpd accept-box disown and the handler-chain leak that remains.

---

## TL;DR

- `bash tests/run.sh` is **green with leak detection ON**: `2111 passed, 0
  failed`. The compiler path is leak-clean (predecessor plan); the remaining
  leak surface is entirely in **spawned fixture programs**, gated by 106
  `requires.no-leak-check` markers.
- The marker sets `ASAN_OPTIONS=detect_leaks=0` for the spawned binary -- but
  **that binary only carries ASan at all when it autolinks `-lturi`** (the
  sanitized `libturi.a`). This gate is the key, non-obvious fact:
  - **58 of the 106 markers are inert** under `run.sh` -- their fixtures build
    *without* ASan (they autolink only a freshly compiled `src/runtime/hamt.c`,
    never `-lturi`), so the marker currently gates nothing.
  - **43 markers are load-bearing** -- their fixtures autolink `-lturi`, get
    ASan, and genuinely leak.
  - **1 marker is stale** (`reactor-fd-writable` is deterministically clean).
  - **3 markers cover the interpreter path** (`cli-interpret-*`,
    `cli-prefix-unique` run `tur interpret`, which *is* sanitized) -- legitimate.
  - **1** (`httpd-mw-compress`) is `requires.spices` and skips when the sibling
    repo is absent.
- **Every active leak is a bounded, process-lifetime allocation** -- fat-closure
  callback boxes and one-time config tables the reactor/httpd server owns for
  its whole life and never frees. None is an unbounded per-request leak.

So there is **no correctness fire here.** The work worth doing is (1) a runtime
ownership change that would let ~43 fixtures drop their markers and restore leak
coverage on the async/server runtime, and (2) cheap marker hygiene.

## How leak detection actually reaches a spawned fixture

This is the piece that makes the marker inventory surprising, so it is worth
stating precisely.

The Debug build compiles `tur` (and `libturi.a`) with
`-fsanitize=address,undefined`. When `tur build <fixture>` links the emitted C,
`src/main.c` only adds `-fsanitize=address,undefined` to the *program's* link
line when `autolink_needs_asan` is true, which it computes by running
`nm <dir>/libturi.a | grep __asan_init` for each `-L<dir>` -- i.e. **only when
the fixture autolinks `-lturi`** (`src/main.c:~1980-2011`, propagation at
`:~2080`).

Consequently:

| Fixture autolinks... | Built binary has ASan? | `requires.no-leak-check` does... |
|---|---|---|
| `-lturi` (reactor/httpd runtime) | **yes** | real work -- suppresses a real leak report |
| only `src/runtime/hamt.c` (cps/panic/hkt/...) | **no** | **nothing** -- there is no LSan to silence |
| nothing (pure `tur build`) | no | nothing |

The interpreter path is different: `tur interpret` / `tur run` execute inside
`tur` itself, which is always sanitized, so a marker on a hook fixture that
shells out to `tur interpret` *is* load-bearing.

Verified empirically: for `cps-backend-cstr`, `panic-catch-unwind-caught`,
`hkt-stdlib-parser-instances` the built binary has zero `__asan_init` symbols
and runs clean under `ASAN_OPTIONS=detect_leaks=1`; for `reactor-timer`,
`httpd-mw-json` it has ASan and reports leaks.

## Marker inventory (106 total)

Measured by building each fixture exactly as `run.sh` does (`TUR_CC_FLAGS=-O2
-std=c99 -Wall -fno-strict-aliasing -Lbuild/src`) and running under
`ASAN_OPTIONS=detect_leaks=1`.

### Inert -- 58 markers (build without ASan; gate nothing under `run.sh`)

| Prefix | Count |
|---|---|
| `cps-backend-*` | 39 |
| `panic-*` | 9 |
| `cps-oracle-*` | 3 |
| `hkt-stdlib-*-instances` | 3 |
| `stackless-catch-unwind-result` | 1 |
| `fat-closure-ascription` | 1 |
| `ascribe-fat-closure-call` | 1 |
| `captureless-autobox` | 1 |

These document a **real but currently-invisible** intent: the CPS lowering
leaks DK continuation-chain nodes per `reset`/`shift` (see
`docs/reported/cps-delimited-dk-node-leak.md`), and the monadic/fat-closure
fixtures leak process-lifetime closure boxes. The leak is real; it just is not
*observable* through `run.sh` today because these programs are not linked
against the sanitized runtime. The markers are therefore **defense-in-depth /
documentation**, not active suppressions.

### Active + leaking -- 43 markers (autolink `-lturi`, ASan on, leak)

| Prefix | Count | Typical leak |
|---|---|---|
| `httpd-*` (`-mw`, `-h`, `-async`) | 30 | 16-88 B; a few larger |
| `reactor-*` | 13 | 16-64 B |

Largest observed (all one-time startup/config, not per-request):

```
24680 B / 4 allocs  httpd-mw-rate-limit   (rate-limiter bucket table, in httpd_..._ratelimit_new)
 3360 B / 210 allocs httpd-mw-fold-many    (one fat-closure box per folded middleware)
  184 B / 10 allocs  httpd-mw-compose-of
  136 B / 3 allocs   httpd-mw-cors-opts
```

Root cause (confirmed with `-O0 -g` symbolized frames): the emitted `main`
allocates a **16-byte fat-closure box** -- `malloc(2 * sizeof(int64_t))` holding
`[fn_ptr, env]` -- for each callback and hands it to
`tur_reactor_add_timer` / `tur_reactor_add_fd` / the httpd middleware chain.
The reactor/server retains the pointer for its whole lifetime and never frees
it. `httpd-mw-rate-limit`'s large block is the limiter's fixed bucket array,
allocated once in its `new` constructor. **All bounded by registration/config
count -- none grows per request.** This matches the documented policy exactly:
"process-lifetime closures the caller never frees (e.g. reactor callbacks)."

### Active but clean -- 1 marker (removed in P1)

`reactor-fd-writable` was deterministically leak-free (3/3 runs clean, `-O0` and
`-O2`); its marker was stale and is removed as part of P1.

### Interpreter path -- 3 markers (legitimate)

`cli-interpret-flag-backcompat`, `cli-interpret-subcommand`, `cli-prefix-unique`
are `hook.sh` fixtures that run `tur interpret`. The interpreter intentionally
never frees its process-lifetime closures/natives, so the marker is correct and
load-bearing.

### Spices-gated -- 1 marker

`httpd-mw-compress` also carries `requires.spices` + `requires.dedicated-runner`;
it `(load ...)`s the sibling `turmeric-spices` zlib and skips when that repo is
absent. Same leak class as the other httpd fixtures when it does run.

## What to tackle (prioritized)

### P1 -- Reactor callback-box ownership -- DONE (9 fixtures)

**Landed.** The reactor now owns the fat-closure callback box handed to each
`tur_reactor_add_*` and `free()`s it at `tur_reactor_free`
(`src/async/reactor.c`):

- `TurReactorSource` gained an `owns_cb` flag, set true by every public
  `tur_reactor_add_fd/timer/interval/signal/chan`. `tur_reactor_free` frees each
  owned box exactly once (deduped, since a program may register one box on
  several sources).
- Ownership is the **default**; callers that manage the box themselves opt out
  with the new `tur_reactor_disown_cb(r, id)`. Two do:
  - the internal fiber **park** registrations, whose cb is an inline
    `LocalFiber` field (`&lf->park_cb_fat`), not a heap box -- freeing it would
    corrupt the heap;
  - the **httpd runtime**, which caches its accept-callback box in
    `hb->accept_clos` / `ha->accept_clos` and frees it in its own teardown
    (`stdlib/httpd.tur`). Without the disown, blanket ownership double-frees it
    and the server aborts -- so the disown is what keeps httpd working, even
    though httpd is not made leak-clean by this change (see below).

Result: the 9 pure-reactor fixtures that leaked genuine reactor cb boxes are now
leak-clean and dropped their markers, verified with leak detection ON:
`reactor-timer`, `reactor-fd-{readable,writable,modify,remove}`,
`reactor-signal`, `reactor-chan-bridge`, `reactor-stop-from-callback`,
`reactor-wake-cross-thread`. Full suite stays green (`2111 passed, 0 failed`).

**What P1 did NOT clean, and why (measured, not assumed):**

- **httpd (all `httpd-*`, ~30 fixtures) -- kept markers.** The httpd leak is a
  16-byte-and-up box **allocated in `main`** that is the user's request handler
  / composed middleware onion closure, which the httpd server owns as its
  handler and never frees. It is *not* a reactor callback box (the handler is
  invoked *by* the accept callback, never separately reactor-registered), so
  reactor ownership does not reach it. Confirmed: with the reactor change +
  httpd disown, `httpd-h1-basic` still reports one 16-byte leak `in main` under
  the suite. (An earlier "httpd looks clean" reading was a probe artifact -- a
  standalone run that never drives a request, so the double-free/leak path is
  not exercised; the httpd fixtures self-drive only under the suite harness.)
  This is the **httpd handler-chain-drop** follow-up.
- **`reactor-fibers-*` (5) -- fixed, see "Fiber-body ownership" below.**
- **`httpd-mw-fold-many`, `httpd-mw-compress` -- kept markers.** Same
  handler-chain leak class as the other httpd fixtures (compress is also
  `requires.spices`).

Key correction to the original plan: the fixtures call the C reactor ABI
**directly** (their own `extern-c`), not the `reactor.tur` `reactor-add-*`
wrappers, so an opt-in wired only into those wrappers reaches nothing --
ownership had to be the reactor's **default** with an explicit opt-out. And the
per-caller ownership split (reactor programs leak their boxes; httpd frees its
own) is exactly why a single blanket free is unsafe without the disown.

Risks handled: shared boxes (deduped free at teardown); borrowed non-heap boxes
(park disown); httpd's self-managed box (httpd disown). ASan address-checking
stayed ON throughout, so any residual double-free/UAF would have surfaced as a
suite failure -- it did not.

### Fiber-body ownership -- DONE (5 fixtures)

**Landed**, mirroring the reactor cb handshake. `tur_local_spawn` now owns the
spawn-body box and `tur_local_fiber_group_free` frees it (deduped); the httpd
async server, whose per-request fibers all share one `ha->body_closure` it frees
itself, opts every such fiber out with the new
`tur_local_disown_body(g, fiber_id)` (`src/async/reactor.c`,
`src/async/local_fiber.h`, `stdlib/httpd.tur`).

One wrinkle beyond the reactor case: a **captureless** fiber body was lowered to
a bare C function pointer, not a heap box (`stdlib/reactor.tur`'s `local-spawn`
took `body : int` with no `^fat`), so `free()`ing it SEGV'd -- and, latently,
the trampoline's fat-pointer dispatch would have mis-run it if the fiber ever
executed. Marking the param `^fat` auto-shims a captureless body into a heap
`{ fatshim, fn }` box, which both makes every body a heap box the group can own
and fixes that latent dispatch bug. `reactor-fibers-stop-mid-run` (which queues
captureless fibers that never run, then frees the group) is the case that
exposed it.

Result: the 5 `reactor-fibers-*` fixtures are leak-clean and dropped their
markers; full suite `2111 passed, 0 failed`; httpd-async fixtures still pass
(the body disown prevents the shared-box double-free).

### P1-next -- httpd handler-chain drop (remaining)

The last active-leak class, process-lifetime and bounded. The server stores the
composed handler / middleware onion (heap capturing-closure boxes) and frees
none of them at `httpd-free`. Needs recursive drop of the handler closure and
its captured `next` chain (there is no drop glue for a captured closure chain
today), plus freeing the rate-limiter bucket table in its destructor. This is
the larger, riskier chunk flagged originally; it would clear ~30 httpd markers
plus `httpd-mw-fold-many` and `httpd-mw-compress`.

Do it behind the existing markers and drop each marker only once its fixture
verifies clean with leak detection ON, so the suite ratchets green
incrementally.

### P2 -- DK continuation-chain node leak (already reported)

`docs/reported/cps-delimited-dk-node-leak.md` -- bounded per-`reset`/`shift`
leak of `dk_shift`/`dk_prompt`/`dk_frame`/`dk_done` nodes, because `DK` is
opaque and emitted code cannot detach a single node to free it. Fix directions
already recorded there (`dk_free_node`, or arena-per-reset). **Low urgency**
precisely because it is currently invisible under `run.sh` (the CPS fixtures do
not link `-lturi`). It becomes P1-adjacent the day the CPS prompt runtime moves
into `libturi.a`, at which point the 43 inert cps markers become active. Worth
fixing at the source rather than relying on the linkage accident.

### P3 -- Marker hygiene (cheap, do anytime)

1. `reactor-fd-writable/requires.no-leak-check` -- **removed** as part of P1
   (deterministically clean; was stale even before the reactor change).
2. **Annotate the inert markers.** 78 of 106 marker files were empty (no
   rationale). At minimum, add a one-line rationale to the inert ones noting
   they are defensive/documentary and *why* they are currently inert (no
   `-lturi` linkage), so a future reader does not assume they are catching a
   live leak. This aligns with the existing convention (`cps-backend-cstr`,
   `hkt-stdlib-parser-instances`, `cli-interpret-flag-backcompat` already carry
   good rationale text).
3. **Do not mass-delete the inert markers.** They are the intended safety net if
   these programs ever start linking the sanitized runtime; removing them would
   silently drop that net. Annotate, keep.

### Optional -- tighten the detection gate

The `autolink_needs_asan` heuristic means leak coverage on a spawned program is
an accident of whether it happens to pull `-lturi`. If we want *uniform*
coverage (so cps/panic/hkt leaks are actually observable), a follow-up could
teach `tur build` to sanitize the emitted program whenever `tur` itself is a
sanitized build (not only when `-lturi` is linked). That would surface the DK
leak and the closure-box leaks everywhere -- valuable, but it turns ~58
currently-quiet fixtures red until P1/P2 land, so it should follow them, not
lead.

## Non-goals

- Auditing every allocation in the runtime. Scoped to the callback/registration
  leak class behind the markers.
- Changing the interpreter's closure-lifetime design (the `cli-interpret-*`
  markers stay).
- Re-verifying the predecessor plan's compiler arena work (done, green).

## Validation checklist

- [x] `bash tests/run.sh` = `2111 passed, 0 failed` with leak detection ON
      (baseline, this branch).
- [x] Marker inventory measured: 58 inert / 43 active-leak / 1 active-clean /
      3 interpreter / 1 spices-gated.
- [x] Active-leak root cause confirmed as fat-closure callback boxes +
      one-time config tables (symbolized `-O0 -g` frames).
- [x] (P1) Reactor callback-box ownership landed (`owns_cb` default +
      `tur_reactor_disown_cb` opt-out for park and httpd). 9 pure-reactor
      fixtures verified leak-clean with detection ON and unmarked; full suite
      `2111 passed, 0 failed`.
- [x] Fiber-body ownership landed (`owns_body` default + `tur_local_disown_body`
      opt-out; `^fat` on `local-spawn` body so captureless bodies are heap
      boxes). 5 `reactor-fibers-*` fixtures verified leak-clean and unmarked;
      full suite `2111 passed, 0 failed`; httpd-async still green.
- [ ] (P1-next) httpd handler-chain drop -- clears ~30 httpd markers +
      `httpd-mw-fold-many`/`-compress`.
- [ ] (P2) DK node leak fixed at source (`dk_free_node`/arena); repro in the
      report is clean under LSan.
- [ ] (P3) `reactor-fd-writable` marker removed; inert markers annotated.

## Appendix: reproduce the inventory

```sh
# Build the sanitized compiler.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j

# For one fixture: build as run.sh does, check ASan presence + leaks.
export TUR_CC_FLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -Lbuild/src"
exe=$(mktemp)
./build/tur build tests/fixtures/reactor-timer/input.tur -o "$exe"
nm "$exe" | grep -c __asan_init                       # >0 => ASan present
ASAN_OPTIONS=detect_leaks=1 "$exe"                    # LSan report if it leaks

# Symbolized frames: rebuild with -O0 -g.
export TUR_CC_FLAGS="-O0 -g -std=c99 -fno-omit-frame-pointer -fno-strict-aliasing -Lbuild/src -Isrc/runtime"
```
