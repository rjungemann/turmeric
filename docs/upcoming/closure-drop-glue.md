# Closure drop-glue -- the single source of truth

> **This is the ONE file for closure env drop-glue. It replaces four scattered
> docs that used to point at each other in a circle.** If you are looking into
> "closure drop-glue", "escaping fat-closure env leak", "httpd middleware chain
> drop", or a `closure-drop-glue` experiment flag: everything you need is here.
> Do not reopen the archived files to re-derive the design -- the design is
> settled and the feature has shipped.
>
> **Nothing here blocks anything else.** The core subsystem is DONE and
> graduated to always-on. The remaining items (below) are independent, each
> annotated with what it blocks (nothing) and what blocks it (nothing). There is
> no cross-file gate to chase.

## State of the world (verify in code, not in prose)

The closure drop-glue subsystem is **implemented and graduated to always-on**
(2026-07-22). This is not a plan for future work -- it is a record of shipped
work plus a short, flat list of optional follow-ups. Ground truth in the tree:

- `src/runtime/experiments.c:135` -- `closure-drop-glue GRADUATED 2026-07-22`.
- `src/runtime/experiments.c:164` -- name is in `GRADUATED[]`; a lingering
  `--enable=closure-drop-glue` is a TUR-W0063 accept-and-warn no-op.
- `src/runtime/globals.h:220` -- the `g_opt_closure_drop_glue` enable bit and
  all 19 codegen gates are removed; the header ABI is unconditional.
- `TUR_CLOSURE_DROP` / `drop_glue_env` / `tur_closure_drop` are live across
  `emit_expr.c`, `emit_fns.c`, `emit_module.c`, `emit_cps_ir.c`,
  `emit_dk_runtime.c`, `elab_fns.c`, `elab_forms.c`, and `src/async/reactor.c`.
- Full suite green at graduation: **2264 passed, 0 failed** (140 `expected.c` +
  3 rc-count `expected.stdout` regenerated to the always-on codegen).

If a future agent reads a banner somewhere claiming this work is "⛔ ACTIVE /
BLOCKING", that banner is stale doc-cruft from before graduation. The code above
is authoritative. Do not restart the investigation.

## What shipped

A heap fat-closure env carries an 8-byte drop-glue header at `env[-1]` (prepend,
so `fat[0]` dispatch and capture-by-field access stay byte-identical).
`TUR_CLOSURE_DROP(h)` recovers `h[-1]` and runs the env's `drop_glue_<env>`,
which walks owning captures and then frees the base allocation. Every fat
representation (`struct __env_N`, `__tur_fatshim` `{shim,orig}`, poly-to-fat
`{shim,fn,env}`) carries the uniform header.

Capture-kind handling in the walk:

- **rc captures** -- retained at capture (`rc_strong_increment`), released in the
  drop-glue (`rc_strong_decrement` + `rc_free_queue_drain`). Aliasing-safe, no
  move analysis needed.
- **`^fat` nested-closure captures** -- identified by `cap->is_fat`; capturing
  MOVES the source (`binding_mark_moved`), so a second capture is `TUR-E0005`
  use-after-move and the env is sole owner. Released via `TUR_CLOSURE_DROP`. This
  is what frees the `(fn [next] (fn [conn] ...))` httpd middleware onion whole.
- **`Drop`-typeclass owned-opaque captures (e.g. `String`)** -- move-only
  (`:affine`, non-Clone) captures are moved in and released through the resolved
  `Drop` instance (`__inst_Drop_drop_<T>`). Used by owned-`String` CORS captures.

Escaping (stored) closures are handled by **Model U** (the holder struct's
generated fn-field drop glue moves and drops the stored closure) and, for
runtime-built chains, by `:affine` `Handler` / `ClosureChain` opaques with `Drop`
instances that auto-drop at let scope exit. All teardown free-sites that used to
bare-free a now-headered handle route through the header-aware release:

- httpd: `httpd-free` / `httpd-async-free` / `router-free` / `httpd-new-tls`
  (`stdlib/httpd.tur`).
- CPS boundary reap: `__dk_reap_ptr` -> `__dk_reap_closure` (`emit_cps_ir.c`).
- catch-unwind / panic thunk box (`emit_expr.c`); struct/ADT fn-field drop glue
  `drop_fnfields_<T>` (`emit_module.c`).
- Reactor / fiber-group owned boxes: `tur_reactor_release_box` gated on the
  runtime `tur_closure_headers_enabled` flag (precompiled libturi C).

Roughly 15 httpd fixtures and the whole async/reactor family are leak-clean, and
their `requires.no-leak-check` markers were dropped.

## Remaining follow-up (all independent, none blocking)

Each item is standalone. **Blocked by: nothing. Blocks: nothing.** Pick up any,
none, or all, in any order. None gates graduation (already done) or the v1 track.

### F1 -- Verify/close the catch-unwind + panic leak markers  (the one real open item)

At graduation the interior-free **crashes** in the catch-unwind / panic teardown
cluster were fixed (routed through `TUR_CLOSURE_DROP`). Whether those paths are
now also **leak-clean** was never confirmed, so these fixtures still carry
`requires.no-leak-check`:

`panic-catch-panic-of`, `panic-catch-unwind-caught`, `panic-catch-unwind-defer`,
`panic-catch-unwind-double`, `panic-catch-unwind-nested`,
`panic-catch-unwind-nested-deep`, `panic-in-handler`, `panic-reset-clears`,
`panic-with-catch-of`, `stackless-catch-unwind-result`.

- **How to close:** on Linux (LSan is Linux-only -- it cannot be checked on
  macOS), run each fixture's program under leak detection with the marker
  removed. If clean, delete the `requires.no-leak-check` marker. If it still
  leaks, the residual free is the caught-box payload / defer-captured closure --
  route it through `TUR_CLOSURE_DROP` (byte-identical for scalar payloads) and
  then drop the marker.
- **Driver:** the fixtures above.
- **Blocks: nothing. Blocked by: nothing.**

### F2 -- Typed-recursive `list<Closure>` drop glue  (honest generalization; no driver)

R3a shipped runtime-chain teardown via hand-written `Drop` instances on `:affine`
`Handler` / `ClosureChain` opaques (`stdlib/httpd.tur`). The honest-but-larger
alternative -- de-erasing `Cons.tail` from `:int` to `(Cons A)` so a typed
`list<Closure>` gets a type-driven recursive drop glue -- was empirically
confirmed **absent** in the compiler and **not needed** for the corpus. It would
require (a) making `emit_adt_byval_drop_glue` recognize affine/closure heads and
a self-recursive owning tail, and (b) a scope-exit trigger category for
owning-ADT let-bindings in `emit_let_value`. Broad blast radius, one corpus
beneficiary.

- **Driver:** none in the corpus. Build only if an owning recursive-ADT-of-closures
  pattern appears that the affine-opaque `Drop` recipe cannot express cleanly.
- **Blocks: nothing. Blocked by: nothing.**

### F3 -- "Model R (refcount)" per-env `__rc`  (build-on-demand; no driver)

A per-env refcount word for closures legitimately owned by two live owners at
once. The move model (Model U) covers the entire current corpus, so this was
never built. It is a heavier ABI change to the `^fat` layout / HKT thunk recovery
/ `tur_poly_fn_t` (audited in `docs/archive/fat-closure-abi-audit-plan.md`).

- **Driver:** none. Build only when a fixture appears that Model U's move-check
  genuinely cannot express (a closure with two concurrent live owners).
- **Blocks: nothing. Blocked by: nothing.**

## Explicitly out of scope (do NOT fold these into closure drop-glue)

These leaks share fixtures or filenames with the closure work but are **not**
closure-env issues. They have their own archived reports; treating them as
"closure drop-glue residual" is exactly the mistake that created the old tangle.

- **httpd request-accessor `cstr` leak** (`httpd-mw-cookie` / `-form`) --
  `httpd-req-cookie` / `-form` return fresh `malloc`'d `cstr` the handler never
  frees. `docs/archive/httpd-request-accessor-cstr-leak.md`.
- **httpd mw-rate-limit state leak** -- the `RLState` counter table is never
  freed. `docs/archive/httpd-mw-rate-limit-state-leak.md`.
- **httpd new-pool failure-handler leak** -- `httpd-new-pool` leaks its `^fat
  handler` on construction-failure paths. `docs/archive/httpd-new-pool-failure-handler-leak.md`.
- **Intentional / unrelated markers** -- `httpd-req-string-opt` (documented
  test-scaffold leak), `httpd-mw-compress` (`requires.spices` skip),
  `cps-backend-heap-adt-return` (non-closure ADT), `cli-*` (unrelated to closures).

## Related but separate track (not part of this file)

`docs/upcoming/cps-backend-ref-scope-exit-drop-plan.md` -- `ref<T>` / owning-local
scope-exit auto-drop under CPS. It rides the same drop discipline but is its own
subsystem with its own open work (P1/P2-single-shot landed; P2-abortive +
P3-multishot crossing still fall back). Left as a separate file on purpose --
folding it in would re-create the sprawl this file exists to end.

## History (archived detail -- read only for context, never for "what to do next")

- `docs/archive/closure-drop-glue-plan.md` -- the full 1300-line development
  record (S1, S2/Model U, Model R slices, R1-R4, all dated progress notes).
- `docs/archive/closure-drop-glue-graduation-blockers.md` -- the 33->3 forced-on
  teardown-crash audit, resolved at R4-prep.
- `docs/archive/escaping-fat-closure-env-leak.md` -- the original bug report this
  subsystem resolved (the `make-scaler` fat-env repro).
- `docs/archive/httpd-middleware-chain-drop-plan.md` -- an alternative
  (chain-reification) approach that was superseded; the general drop-glue feature
  drained the middleware onion for free, so this was never implemented.
