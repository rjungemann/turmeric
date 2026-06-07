# Stdlib Inline-C Tier-3 Triage

> **Status:** Triage complete (2026-06-04). No rewrites committed here.
> **Type:** classification artifact for Phase 3 of
> [stdlib-inline-c-deworkaround-plan.md](stdlib-inline-c-deworkaround-plan.md)
> **Parent plan:** stdlib-inline-c-deworkaround-plan.md (Tier 3)

---

## Purpose

Phase 3 of the inline-C de-workaround plan asks us to *triage* -- not yet
rewrite -- the six Tier-3 modules with disproportionately heavy inline-C. For
each module this doc classifies every inline-C block as either:

- **GLUE** -- libc/syscall bridge (sockets, pthread, malloc, clock), ABI /
  fat-closure dispatch, atomics / memory-ordering, raw pointer / tag-bit
  manipulation, or codegen intrinsic. **Keep** per design principle 4.
- **DOMAIN** -- pure data shuffling / control logic (cons-walks, string
  building, arithmetic, branching) that idiomatic Turmeric can express.
  **Rewrite candidate.**

The plan's Phase 3 step 2 originally said "open a tracking issue per module";
we are tracking in-repo here instead. Sequencing (step 3) still holds: each
rewrite waits behind the relevant opaque-handle / advanced-typing phase so it
can lean on real types instead of `:int`/`ptr<void>`.

---

## Summary

| Module | Blocks | GLUE | DOMAIN | Rewrite verdict |
|---|---:|---:|---:|---|
| `stdlib/threadpool.tur` | 16 | 16 | 0 | **None.** All pthread/queue glue. Leave as-is. |
| `stdlib/chan.tur` | 11 | 11 | 0 | **None.** All pthread/ring-buffer glue. Leave as-is. |
| `stdlib/future.tur` | ~24 | ~21 | 3 | Small: `future-map`/`future-then`/`future-join` combinators. Blocked on opaque-handle `Future`/`FutureCell` + fat-closure stdlib support. |
| `stdlib/parsec.tur` | ~24 | ~22 | 2 | Small: `many-c-impl`/`pstring-c-impl`. Blocked on advanced-typing T2 (`Monad [Parser]`). |
| `stdlib/json.tur` | 30 | ~28 | 2 | Small: `json-enc-node`/`json-dec-parse-value`. Blocked on `defopaque Value` (tag-bit access) + buffer-builder primitive. |
| `stdlib/httpd.tur` | 71 | ~31 | ~40* | Largest opportunity, but most DOMAIN blocks are *blocked* on opaque Request/Response/Middleware handle types + a typed header list. |

\* The httpd DOMAIN count is high but soft: the bulk are trivial struct-field
accessors on opaque `ptr<void>` handles. They are "domain" in spirit yet still
require raw pointer access **until** the opaque-handle plan gives Request /
Response sealed types with field accessors. They are rewrite *targets*, not
rewrite-*now*s.

**Headline:** concurrency primitives (`threadpool`, `chan`, the synchronizing
core of `future`) are correctly inline-C and should not be touched. The real
de-workaround surface is concentrated in `httpd.tur`, and almost all of it is
gated on the opaque-handle plan's Tier 1 landing first.

---

## Per-module triage

### `stdlib/threadpool.tur` -- 16 blocks, 0 DOMAIN

Every block is a pthread / ring-buffer / atomic primitive: queue create / push
/ pop / close / free, worker entry points (`tp-worker`, `tp-dyn-worker`), pool
lifecycle (`thread-pool-new`/`-shutdown`/`-free` and the dynamic variants),
and task submission. No pure data-shuffling blocks. **Verdict: keep all.**

### `stdlib/chan.tur` -- 11 blocks, 0 DOMAIN

All blocks manage pthread mutex / condvar synchronization, ring-buffer state
under lock, `cond_timedwait` + `clock_gettime` deadlines, and cooperative
cancellation checks (`tur_thread_cancel_requested`). `chan-*` and
`async-chan-*` are symmetric. **Verdict: keep all.**

### `stdlib/future.tur` -- ~24 blocks, 3 DOMAIN

GLUE (keep): cell alloc/free with `pthread_mutex_init`/`cond_init`, refcount
via `__atomic_*`, `promise-fulfill`/`-fail` (mutex+broadcast), `future-get`
(`cond_wait`), the `future-race*` / `future-all*` / `future-any*` await
orchestration, and the timer-thread spawns (`future-timeout`,
`future-with-timeout`).

DOMAIN (rewrite candidates):

| Function | Loc | Reason |
|---|---|---|
| `future-map` | `stdlib/future.tur:457` | fat-closure unpack + result-cell setup; control flow around a `fn` application |
| `future-then` | `stdlib/future.tur:498` | fat-closure unpack + conditional cell alloc; pure control flow |
| `future-join` | `stdlib/future.tur:668` | Tuple2 alloc + field copy; pure data manipulation |

**Rewrite plan / blockers:** these combinators wrap genuine concurrency
primitives in data-shuffling that could be Turmeric, but they currently
hand-unpack fat closures (`intptr_t` casts + array indexing) and hand-build
`FutureCell`/`Tuple2`. A clean rewrite needs (a) the opaque-handle plan to
expose `FutureCell` constructors / `Future` as a sealed type so cells aren't
C-only artifacts, (b) stdlib-level fat-closure dispatch (the `^fat` ABI used by
`httpd-mw-apply`), and (c) `future-join` to consume `tuple2` from
`stdlib/tuple.tur` instead of raw malloc. Sequence after opaque-handle Tier 1.

### `stdlib/parsec.tur` -- ~24 blocks, 2 DOMAIN

GLUE (keep): monad plumbing (`mzero`/`mreturn`/`mplus`/`mbind`), the `Cell`
list ABI (`bt-nil`/`bt-cons`/`bt-length`), `Pair`/`Input` constructors and
accessors, fat-closure dispatch (`apply-fat`/`apply-parser`), and result
extraction (`run-parser-full-c`, `parse-value`).

DOMAIN (rewrite candidates):

| Function | Loc | Reason |
|---|---|---|
| `pstring-c-impl` | `stdlib/parsec.tur:417` | bounded string comparison + `Input` advance; expressible as a `pchar` composition |
| `many-c-impl` | `stdlib/parsec.tur:552` | greedy repetition: accumulate-reversed then reverse; pure list shuffling |

**Rewrite plan / blockers:** both want a Turmeric loop that repeatedly calls
`apply-parser` and inspects success, plus forward-order list accumulation.
Cleanest once advanced-typing **T2** lands `Monad [Parser]`, so `many`/`pstring`
become combinator expressions over `bind`/`pure` rather than tight C loops.
Watch for a performance regression vs. the current loops; fix in codegen
(inline the dispatch), not by reverting.

### `stdlib/json.tur` -- 30 blocks, 2 DOMAIN

GLUE (keep): node allocation with tag embedding (`json/null`..`json/float`,
`json/string` via `strdup`), Vec/array realloc, object linked-list nodes,
tag-bit extraction + field access accessors, encode buffer management
(`json-enc-ensure`/`-append-*`/`-str`), lexical scan (`json-dec-skip-ws`,
`json-dec-parse-string`), and recursive free.

DOMAIN (rewrite candidates):

| Function | Loc | Reason |
|---|---|---|
| `json-enc-node` | `stdlib/json.tur:435` | tag-dispatch + recursive encode; tree traversal + formatting |
| `json-dec-parse-value` | `stdlib/json.tur:547` | recursive descent over JSON grammar; parser-combinator-shaped |

**Rewrite plan / blockers:** encoding wants a Turmeric string-builder and a
typed tree walk; decoding wants the parsec combinators. Both are blocked on a
`defopaque Value` wrapping the `[type-tag; payload]` representation so the
dispatch can be Turmeric `match` instead of raw int64 tag reads -- and on a
buffer-builder primitive to avoid manual `realloc`. Lower priority: the json
core is mostly legitimate tag/pointer glue.

### `stdlib/httpd.tur` -- 71 blocks, ~31 GLUE / ~40 DOMAIN

`httpd-mw-fold` is **excluded** (de-workaround'd in Phase 2).

GLUE (keep, ~31): the socket / reactor / pthread workhorses -- `httpd-handle`,
`httpd-worker`, `httpd-accept-cb`, `httpd-new-pool` (socket/bind/listen/fcntl +
`pthread_create`), `httpd-run`/`-stop`/`-free`, the full async-fiber path
(`httpd-async-fiber-body`, `httpd-async-accept-cb`, `httpd-new-async-*`,
`httpd-run-async`, the `httpd-await-*` fiber parks), `getsockname` port
readers, TLS registration/dispatch, and the fat-closure dispatchers
`httpd-call` / `httpd-mw-apply`.

DOMAIN (rewrite candidates, ~40 -- grouped by theme):

- **Request/response field accessors (~15)** -- `httpd-req-method`/`-path`/
  `-version`/`-body`/`-body-len`, `httpd-resp-status-get`/`-status!`/`-body!`/
  `-body-bytes!`, `httpd-part-*` (`stdlib/httpd.tur:930`-`1024`, `2075`-`2123`).
  Trivial struct-field reads/writes on opaque pointers.
- **Header / cookie / attr manipulation (~7)** -- `httpd-req-header`/`-header?`,
  `httpd-resp-header!`/`-header-add!`, `httpd-req-cookie`, `httpd-set-cookie!`,
  `httpd-req-attr` (`:1068`-`1374`, `:3374`). Cons-walks + name matching +
  serialization.
- **Body parsing (~5)** -- `httpd-req-form`, `httpd-req-json`,
  `httpd-mw-json-body-precheck`, `httpd-req-file`, `httpd-req-multipart-parse`
  (`:1434`-`2053`).
- **Routing / params (~5)** -- `router-new`/`-add`/`-free`/`-dispatch`,
  `httpd-param` (`:2814`-`2982`). Linked-list + path-pattern matching.
- **Middleware helpers (~8)** -- CORS preflight/decorate, basic-auth
  check/challenge + `cstr-eq-const-time`, content-length, log formatting,
  rate-limit hash table, static MIME/file serving (`:1619`-`3626`).

**Rewrite plan / blockers:** this is the largest surface but the most gated.
The accessors and header/cookie/attr blocks all need the **opaque-handle plan**
to land sealed `Request` / `Response` / `Middleware` types with real field
accessors and a typed header list before they can be idiomatic Turmeric --
today they are raw `ptr<void>` field pokes. Body/routing/middleware blocks
additionally want string-slicing + buffer-building primitives. A couple
nominally-DOMAIN blocks still bottom out in syscalls (`httpd-req-remote-ip`
-> `getpeername`/`inet_ntop`, `httpd-mw-ratelimit-check` -> `clock_gettime`)
and stay partly GLUE. **Sequence: strictly after opaque-handle Tier 1.**

---

## Sequencing (Phase 3 step 3)

No rewrites are funded by this doc. The dependency order is:

1. **opaque-handle plan Tier 1** (Request/Response/Middleware/Future/Chan
   sealed types) -> unblocks the httpd accessor/header surface and the
   `future` combinators.
2. **advanced-typing T2** (`Monad [Parser]`) -> unblocks `parsec` `many`/
   `pstring`.
3. **`defopaque Value` for json** -> unblocks the two json domain blocks.

Until then, the only Tier-3 modules that are *fully settled* are
`threadpool.tur` and `chan.tur` (0 rewrite candidates -- correct as-is).
