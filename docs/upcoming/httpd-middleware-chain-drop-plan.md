# Plan: Reify the httpd middleware chain so it can be freed

> **Status: RESOLVED-SUPERSEDED (2026-07-22). The middleware-onion leak this plan
> targets is fixed -- but by the general closure-drop feature this plan explicitly
> rejected, NOT by chain reification. Phases 1-5 below were never implemented and
> are withdrawn.**
>
> What actually happened: `closure-drop-glue` (the compiler drop-glue header ABI --
> `docs/upcoming/closure-drop-glue-plan.md`) shipped behind `--enable=` and then
> **graduated to always-on 2026-07-22** (`src/runtime/experiments.c`). `httpd-free`
> / `httpd-async-free` now drop the composed handler via `TUR_CLOSURE_DROP`
> (`stdlib/httpd.tur:971-979`, `:2930`), whose drop-glue walk frees every inner
> `next` box down the onion; the runtime-built `httpd-mw-fold` chain auto-drops via
> an `:affine` `ClosureChain`/`Handler` opaque with a `Drop` instance
> (`stdlib/httpd.tur:3414`, `:3477`); and the `mw-rate-limit` bucket table is freed
> via a file-scope registry + one-time atexit hook (commit `301466236`). No
> `MwChain` / `mw-chain-dispatch` / `mw-chain-free` was ever built (0 hits in tree).
>
> Outcome vs the "Why C rather than a general closure-drop feature" bet below: that
> bet -- that drop-glue's cost was "not justified by this one bounded leak" -- was
> overtaken, because a second, independent need for drop-glue appeared and the
> feature shipped anyway. Its walk drained this onion for free.
>
> **16 of the 17 markers are dropped.** Every listed fixture
> (`httpd-mw-*` incl. `-fold-many`, `httpd-async-mw-compose`, `httpd-h5-tls`,
> `httpd-h7-middleware`) is now leak-clean with NO `requires.no-leak-check`. The
> lone remaining marker is `httpd-mw-compress`, and it stays only because it is
> `requires.spices` (zlib absent) -- the onion-leak rationale on it is vestigial.
> Per the CLAUDE.md archive convention this resolved plan is a candidate to move to
> `docs/archive/`. Phases 1-5 below are retained as a withdrawn design record.
>
> **(Prior status, now historical.)** Accepted-and-deferred; re-verified
> 2026-07-19 as still deferred with phases 1-5 unstarted. That deferral was
> resolved by the closure-drop-glue route above two days later.
> **Type:** Stdlib redesign (`stdlib/httpd.tur`); no compiler change.
> **Related:**
> - `docs/leak-detection-followups-plan.md` -- parent plan; this is its last
>   open item ("httpd middleware-onion inner-layer drop").
> - `stdlib/httpd.tur` -- `compose-middleware` / `httpd-mw-fold` /
>   `compose-middleware-of`, the ~28 `mw-*` middleware, and
>   `httpd-free` / `httpd-async-free` / `router-free` (where the outer-handler
>   free already lives).

---

## Current state: D (accepted bounded leak)

The composed-handler leak is **accepted as-is for now**. It is bounded
(one heap box per middleware layer per server), process-lifetime (reachable at
exit, freed by no one), and has **no correctness impact** -- the same category
as the interpreter's process-lifetime closures already carved out repo-wide.

Concretely, "D now" is in effect:

- The parent plan's outer-handler drop landed: `httpd-free`, `httpd-async-free`,
  and `router-free` free the `^fat` handler / route-handler box, which fully
  cleans *bare*-handler fixtures (12 unmarked).
- The 17 composed-handler fixtures + `httpd-mw-fold-many` + (spices-gated)
  `httpd-mw-compress` keep `requires.no-leak-check`, each now carrying a
  rationale that names this as an intentional bounded leak and points here.

No further code change is required to *hold* this state. The rest of this
document is the plan to *resolve* it when the time is right.

## The leak, precisely

`compose-middleware` / `httpd-mw-fold` build a request handler as an **onion of
closures**: each middleware layer

```turmeric
(defn mw-json-body [^fat next : int] : ptr<void>
  (let [_n next]
    (fn [c : ptr<void>] : nil ... (httpd-call _n c))))   ; captures _n = next
```

returns a closure that **captures the downstream `next` box**. The composed
handler is a chain of heap env structs
`mw1_env { __fn; _n = mw2_env { __fn; _n = ... base } }`. The outer-handler free
reclaims the outermost layer; every inner layer leaks.

## Why the outer-handler free can't just recurse

Two constraints rule out draining the onion from the existing C teardown:

1. **The chain is type-erased.** `httpd-mw-fold [base : int mws : int] : int`
   and every `mw-*` return `:ptr<void>`/`:int`. By the time the handler reaches
   `httpd-new`, its precise nested-closure type -- the only thing that could
   drive a matching recursive free -- is gone. For `httpd-mw-fold-many` the
   chain length is a *runtime* value, so no static type exists even in principle.
2. **The drop is C-initiated from an opaque box.** `httpd-free` holds only
   `int64_t handler`; it cannot tell a captured *closure* field from a captured
   *value*, and the emitted `struct __env_N` layout is not a stable ABI.

So the fix must make the chain into something the server **owns and can walk**,
rather than an opaque closure it can only call.

## The plan: C -- reify the middleware chain (later)

Stop encoding the chain as nested closures. Represent it as an explicit heap
`MwChain` node list the server owns.

- Each node holds the layer's `mw_fn`, that layer's own config captures, and a
  `next` node pointer. Crucially, `next` lives in the **node**, not captured
  inside the layer closure.
- The layer fn takes `(c, next)` and the dispatcher supplies `next` while
  walking the node list -- so each layer box now captures only its *config*
  (usually nothing, or leaf values like a header name or a byte cap).
- `httpd-free` / `httpd-async-free` walk the node list and free each node and
  its layer box. Because a layer box captures only leaf config, freeing it is a
  **leaf free -- no recursion, no drop glue**.

Residual to handle in the same pass: a middleware whose config is itself a
closure (`mw-basic-auth`'s `^fat verifier`) still holds one nested box. Reify
that into the node too -- store the verifier box explicitly and free it when the
node is freed -- and the chain drains fully.

### Why C rather than a general closure-drop feature

The alternative is compiler-generated **closure drop glue** -- a per-closure-type
recursive free, reached from C via an `__fn -> drop-glue` registry (keeps the
box layout) or an embedded drop-fn word (changes the closure ABI, large snapshot
churn). That is the correct *general* answer and would fix every closure leak,
not just this onion -- but its cost (capture-kind analysis at closure synthesis;
a global registry with thread-safety and double-drop care; or an ABI change) is
not justified by this one bounded leak. If closure leaks ever turn up
pervasively *beyond* httpd, revisit it then, behind
`--enable=closure-drop-glue` per the experimental-features rule
(`src/runtime/experiments.c`); until such a second, independent need appears, C
is the right-sized fix because it is **local to `stdlib/httpd.tur` and needs no
compiler or ABI change**.

C's cost is real but contained: it redesigns the middleware *representation* and
the ~28 `mw-*` functions + `compose-middleware` / `httpd-mw-fold` + the dispatch
path, and it churns the httpd fixtures' `expected.c`. That is exactly why it
should **ride the next middleware-API revision** rather than be spent solely on
the leak -- the churn is then amortized against work already touching these
functions.

## Phased plan (when C lands)

1. Define `MwChain` (`defstruct`/`defopaque`): `{ mw_fn : int; cfg... ;
   next : ptr<MwChain> }`. Adjust `mw-*` to return a chain-node builder rather
   than a `next`-capturing closure; the layer fn takes `(c, next)`.
2. Rewrite `compose-middleware` / `httpd-mw-fold` / `compose-middleware-of` to
   thread `MwChain` nodes instead of folding closures. Preserve apply order
   (leftmost = outermost).
3. Add `mw-chain-dispatch` that walks nodes supplying `next`, and point the
   server handler at it.
4. Add `mw-chain-free` (walk + free each node, its layer box, and any reified
   closure config such as the basic-auth verifier); call it from `httpd-free` /
   `httpd-async-free` alongside the existing outer-handler free. Fold in the
   one-time `mw-rate-limit` bucket-table free here too.
5. Regenerate the httpd fixture snapshots. Drop each of the 17 markers only once
   its fixture verifies clean with leak detection ON, so the suite ratchets
   green incrementally.

## Risks

- **Middleware-behavior regressions.** Moving `next` from a capture to a
  dispatcher-supplied argument changes the request path; the existing httpd
  fixtures (behavior, not just leaks) are the guard.
- **Double-free / shared ownership.** A handler mounted on several routes, or an
  aliased chain, must be freed once. Giving the server *sole* ownership of the
  node list is what sidesteps this -- keep composition producing a fresh node
  list per server, mirroring the dedup/disown discipline the reactor and
  fiber-body ownership work already used.
- **Snapshot churn.** Item 5 regenerates many `expected.c`; do it in the same
  change, not a follow-up (per the repo's fixture-churn rule).

## Validation

- The 17 appendix fixtures build clean with leak detection ON and drop their
  markers; `bash tests/run.sh` stays green (`~2111 passed, 0 failed`).
- ASan address-checking ON throughout (as in the parent work) so any
  double-free / UAF from a mis-scoped free surfaces as a suite failure.
- `httpd-mw-fold-many` (runtime-length chain) is the stress case -- its ~210
  layer boxes must all drain.

## Appendix: the currently-marked fixtures

`httpd-mw-{basic-auth, basic-auth-attr, basic-auth-noncapture, body-size,
compose, compose-of, cookie, cors, cors-opts, form, json, log, rate-limit,
static}`, `httpd-h{5-tls, 7-middleware}`, `httpd-async-mw-compose`, plus
`httpd-mw-fold-many` and (spices-gated) `httpd-mw-compress`. Each marker now
carries a rationale naming this as an intentional bounded leak and pointing
here. `httpd-mw-rate-limit` also holds a one-time ~24 KB bucket table freed in
its own destructor -- fold that free into step 4.
