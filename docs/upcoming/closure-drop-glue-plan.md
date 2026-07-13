# Plan: Dropping the httpd middleware-closure onion

> **Status:** Proposed (design)
> **Type:** Runtime memory-management -- compiler codegen OR stdlib redesign
> **Related:**
> - `docs/leak-detection-followups-plan.md` -- parent plan; this is its last
>   open item ("httpd middleware-onion inner-layer drop").
> - `stdlib/httpd.tur` -- `compose-middleware` / `httpd-mw-fold`, the ~28
>   `mw-*` middleware, `httpd-free` / `httpd-async-free` / `router-free`.
> - `src/compiler/emit_module.c` -- `emit_adt_byval_drop_glue` (`drop_glue_*`,
>   `walk_glue_*`), the existing ADT drop-glue this would extend.
> - `src/compiler/elab_fns.c` (~4369) -- closure env-struct synthesis
>   (`__env_N { __fn; <captures> }`).

---

## The leak, precisely

`compose-middleware` / `httpd-mw-fold` build a request handler as an **onion of
closures**: each middleware layer

```turmeric
(defn mw-json-body [^fat next : int] : ptr<void>
  (let [_n next]
    (fn [c : ptr<void>] : nil ... (httpd-call _n c))))   ; captures _n = next
```

returns a closure that **captures the downstream `next` box**. So the composed
handler is a chain of heap env structs
`mw1_env { __fn; _n = mw2_env { __fn; _n = ... base } }`, each a `malloc`'d
`struct __env_N`. The server stores the outer box and, before this work, freed
none of it.

The parent plan's outer-handler drop (`httpd-free`/`httpd-async-free`/
`router-free` now `free()` `hb->handler` / `ha->handler` / `route->handler`)
reclaims the **outermost** layer. That fully cleans a *bare* handler (one box);
for a composed handler it leaves every inner layer leaked. 17 fixtures still
leak and keep `requires.no-leak-check` (see appendix).

The leak is **bounded and process-lifetime** (one box per middleware layer per
server, freed by no one, reachable at exit) -- no correctness impact. It is a
coverage gap, not a fire.

## Why this is hard (the two constraints that kill the easy fixes)

1. **The chain is type-erased.** `httpd-mw-fold [base : int mws : int] : int`
   and every `mw-*` returns `:ptr<void>`/`:int`. At the point the handler
   reaches `httpd-new`, its static type is just "a fat closure (`:int`)". The
   precise nested-closure type -- the only thing that would let the compiler
   synthesize a matching recursive free -- has been erased. For
   `httpd-mw-fold-many` the chain length is a *runtime* value, so no static type
   exists even in principle.

2. **The drop is C-initiated from an opaque box.** Teardown runs in
   `httpd-free` (inline C) holding only `int64_t handler` -- a raw box pointer
   with no attached type or layout. The C code cannot tell a captured *closure*
   field (must be recursively dropped) from a captured *value* field (must not),
   and the emitted `struct __env_N` field order is not a stable ABI it could
   hard-code against.

Together these mean: **any complete fix needs per-box drop information available
at runtime, dispatched dynamically as the chain is walked** -- either carried by
each box, looked up from each box, or made explicit in a structure the server
owns.

## Options

### A. Runtime closure drop glue, dispatched via a fn->drop registry

Extend the existing ADT drop-glue machinery to closures. For every closure env
type that captures an owned heap reference (another closure, an `rc`/`ref`
field), the compiler emits `drop_glue___env_N(void *box)` that recursively drops
those captured fields (calling *their* drop glue) and then `free(box)` --
exactly mirroring `emit_adt_byval_drop_glue`. A global table maps a closure's
`__fn` pointer (already at `box[0]`) to its drop-glue fn; closure construction
registers the pair once. `httpd-free` calls a runtime `tur_closure_drop(box)`
that looks up `box[0]` and invokes the glue, recursing through the onion.

- **Pro:** general -- fixes every closure leak in the language, not just httpd;
  no change to the closure box layout (dispatch still reads `box[0]`).
- **Con:** a global registry (populate on every closure creation, look up on
  drop) adds cost and a thread-safety surface; "which captures are owned" needs
  a capture-kind analysis the closure path does not do today; double-drop safety
  (a box reachable from two owners) needs care. Broad blast radius.

### B. Runtime closure drop glue, dispatched via an embedded drop-fn word

As A, but instead of a registry, widen the closure box to carry its drop-fn
(e.g. `box[1] = drop_glue` for capturing closures, captures shifted by one).
`tur_closure_drop` reads the drop-fn straight from the box.

- **Pro:** no global table; drop dispatch is a direct call.
- **Con:** changes the closure box **ABI** -- every construction, dispatch, and
  `^fat` shim site, plus a large `expected.c` snapshot regen. Highest churn of
  all options. Only worth it if closure drop becomes a first-class, pervasive
  need.

### C. Reify the middleware chain (stdlib-local, no compiler change)

Stop encoding the chain as nested closures. Represent it as an explicit, heap
`MwChain` node list the server owns: each node holds the layer's `mw_fn` and its
own config captures, plus a `next` node pointer. The dispatcher walks nodes,
supplying `next` as an argument rather than a capture. `httpd-free` walks the
node list and frees each node and its layer box.

Because each layer box now captures only its *config* (usually nothing, or leaf
values), freeing it is a leaf free -- no recursion needed for the common case.
Residual: middleware that capture *another closure* as config
(`mw-basic-auth`'s `^fat verifier`) still hold one nested box; reify that into
the node too (store the verifier box explicitly and free it), and the chain is
fully drained.

- **Pro:** localized to `stdlib/httpd.tur`; **no compiler/ABI change**; no
  registry; clears all 17 markers. Lowest systemic risk.
- **Con:** redesigns the middleware representation and the ~28 `mw-*` functions
  + `compose-middleware`/`httpd-mw-fold` + the dispatch path; churns the httpd
  fixtures' `expected.c`. Solves *only* httpd, not closures in general.

### D. Accept the bounded leak (null option)

Keep the 17 markers; document the onion inner layers as an intentional,
bounded, process-lifetime leak (same category as the interpreter's
process-lifetime closures already carved out repo-wide).

- **Pro:** zero risk, zero churn; honest about a leak with no correctness cost.
- **Con:** leaves real leak coverage off for the composed-handler path -- a
  *new* leak there stays masked by the blanket marker.

## Recommendation

Sequence by cost against the v1 track, not by purity:

1. **Now: D.** These are bounded, process-lifetime leaks with no correctness
   impact; the outer-handler drop already cleared the 12 fixtures that were
   cheap. Do not open a language-wide ABI change (A/B) or a 28-function
   middleware redesign (C) solely to reclaim ~1 box per middleware layer at
   process exit. Keep the 17 markers with the accurate rationale already on
   them.

2. **When the middleware API is next revised anyway: C.** The reified chain is
   the right *scoped* fix and rides along a redesign that is already touching
   these functions and their snapshots, so the churn is amortized rather than
   spent solely on the leak. It needs no compiler work and carries the least
   systemic risk.

3. **Only if closure leaks become pervasive beyond httpd: A.** A general,
   registry-based closure drop glue is the correct systemic answer, but its cost
   (capture-kind analysis, a global registry, double-drop safety) is justified
   only when closures leak in many places, not just this one onion. Prefer A
   over B -- keep the closure box layout stable. If pursued, ship it behind
   `--enable=closure-drop-glue` per the experimental-features rule
   (`src/runtime/experiments.c`, plan in `docs/upcoming/`) until it graduates,
   since it touches codegen and carries a known cost.

The recommendation is deliberately **not** "build the big feature": the leak
does not earn it. Escalate A→ only when a second, independent closure-leak
need appears.

## If C is chosen -- phased plan

1. Define `MwChain` (`defstruct`/`defopaque`): `{ mw_fn : int; cfg... ; next :
   ptr<MwChain> }`. Adjust `mw-*` to return a chain node builder rather than a
   `next`-capturing closure; the layer fn takes `(c, next)`.
2. Rewrite `compose-middleware` / `httpd-mw-fold` / `compose-middleware-of` to
   thread `MwChain` nodes instead of folding closures.
3. Add `mw-chain-dispatch` that walks nodes, and point the server handler at it.
4. Add `mw-chain-free` (walk + free each node, its layer box, and any reified
   closure config like the basic-auth verifier); call it from `httpd-free` /
   `httpd-async-free` alongside the existing handler free.
5. Regenerate the httpd fixture snapshots; drop each of the 17 markers only once
   its fixture verifies clean with leak detection ON, so the suite ratchets
   green incrementally.

## If A is chosen -- phased plan

1. Capture-kind analysis: at closure synthesis (`elab_fns.c`), classify each
   captured field as owned-heap (closure / `rc` / `ref`) vs value. Set a
   `needs_drop_glue` bit on the env type.
2. Emit `drop_glue___env_N` for those, mirroring `emit_adt_byval_drop_glue`
   (recurse into owned closure captures via their glue, then `free`).
3. Runtime `tur_closure_drop(box)` + a `__fn -> drop_glue` registry; register at
   construction. Guard double-drop.
4. `httpd-free` etc. call `tur_closure_drop(handler)` instead of the bare
   `free`. Gate the whole feature behind `--enable=closure-drop-glue`.
5. Fixture regen; drop markers as each path verifies clean.

## Risks

- **Double-free / shared boxes.** A closure box reachable from two owners
  (aliased handler, a middleware mounted on several routes) must be dropped
  once. A/B need refcount-or-dedup at the drop root; C sidesteps it by giving
  the server sole ownership of the node list. (This is the same hazard the
  reactor/ fiber ownership work already navigated with dedup + disown.)
- **ABI churn (B).** A layout change regenerates every closure snapshot -- avoid
  unless the feature is first-class.
- **Registry cost/threads (A).** Populate-on-create / lookup-on-drop is per
  closure; keep it off the hot path and thread-safe.
- **Middleware-behavior regressions (C).** Moving `next` from capture to
  argument changes the dispatch path; the existing httpd fixtures are the guard.

## Validation

- The 17 appendix fixtures build clean with leak detection ON and drop their
  markers; `bash tests/run.sh` stays green (`~2111 passed, 0 failed`).
- ASan address-checking ON throughout (as in the parent work) so any
  double-free / UAF from a mis-scoped drop surfaces as a suite failure.
- `httpd-mw-fold-many` (runtime-length chain) is the stress case -- its ~210
  layer boxes must all drain.

## Appendix: the 17 still-marked fixtures

`httpd-mw-{basic-auth, basic-auth-attr, basic-auth-noncapture, body-size,
compose, compose-of, cookie, cors, cors-opts, form, json, log, rate-limit,
static}`, `httpd-h{5-tls, 7-middleware}`, `httpd-async-mw-compose`, plus
`httpd-mw-fold-many` and (spices-gated) `httpd-mw-compress`. `httpd-mw-rate-limit`
also holds a one-time ~24 KB bucket table freed in its own destructor -- fold
that free into whichever option lands.
