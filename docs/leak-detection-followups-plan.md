# Plan: Leak-Detection Follow-ups -- the `requires.no-leak-check` surface

> **Status:** Proposed
> **Last Updated:** 2026-07-12
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
> - `src/runtime/reactor.c`, `src/runtime/httpd*.c` -- the callback-registration
>   sites that leak.

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

### Active but clean -- 1 marker (removal candidate)

`reactor-fd-writable` is deterministically leak-free (3/3 runs clean, `-O0` and
`-O2`). Its marker is stale.

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

### P1 -- Reactor/httpd callback ownership (the real coverage win)

Give the async runtime ownership of the fat-closure boxes it is handed, and
free them at teardown. Concretely:

1. `tur_reactor_add_timer` / `add_fd` / `add_signal` / channel-bridge and the
   httpd middleware-chain registration record the callback box pointer in the
   registration struct they already keep.
2. On unregister (`fd-remove`, timer fire-once completion, fiber cancel) and on
   `reactor_free` / server shutdown, free the owned box.
3. The httpd rate-limiter frees its bucket table in its destructor.

Payoff: ~43 fixtures drop `requires.no-leak-check`, and the reactor/httpd
runtime regains real LSan coverage in `run.sh` -- today a *new* leak on that
path is masked by the blanket marker.

Risks / gates:
- **Shared/aliased boxes.** A closure box reused across multiple registrations
  (e.g. one handler mounted on several routes) must be freed exactly once.
  Audit whether any box is shared before making the reactor the owner; if so,
  refcount or clone-on-register.
- **Fire-and-forget vs. persistent callbacks.** One-shot timers can free on
  fire; fd watchers free on remove; fibers free on cancel/exit. Get the
  lifetime right per registration kind or the fix trades a leak for a
  use-after-free (keep ASan address-checking on throughout to catch it).
- Do this behind the existing per-fixture markers: remove a fixture's marker
  only after its specific path is owned+freed, so the suite ratchets green
  incrementally.

Effort: medium. This is a runtime-ownership change threaded through several
registration sites, not a one-liner.

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

1. **Delete `reactor-fd-writable/requires.no-leak-check`** -- deterministically
   clean; keeping it silently disables leak detection on a fixture that does not
   need it. (Re-verify once under `run.sh`'s exact flags before deleting.)
2. **Annotate the 58 inert markers.** 78 of 106 marker files are empty (no
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
- [ ] (P1) Per registration kind: reactor/httpd own+free the box; drop that
      fixture's marker; suite stays green with detection ON.
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
