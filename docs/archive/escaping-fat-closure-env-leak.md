# Escaping fat closure's captured-env struct is never freed

> # ✅ RESOLVED / ARCHIVED (2026-07-23). Superseded by the single source of truth:
> # docs/upcoming/closure-drop-glue.md
> #
> # The closure drop-glue subsystem shipped and GRADUATED to always-on
> # (2026-07-22): experiments.c:135/164, globals.h:220. The minimal repro in this
> # report (`make-scaler` fat-env) is fixed. The "ACTIVE / BLOCKING" banner below
> # is stale pre-graduation cruft -- DO NOT act on it; it is retained only as the
> # historical record. Any remaining follow-up is tracked in the single file above.

> # [HISTORICAL, STALE] ⛔ ACTIVE / BLOCKING -- this report is the exit gate for real work in progress.
>
> The fix is NOT deferred and NOT waiting on anything else. It is
> [docs/upcoming/closure-drop-glue-plan.md](../upcoming/closure-drop-glue-plan.md),
> which is an ACTIVE/BLOCKING build directive. **All other track work is
> blocked until the walk-glue lands and the leak-suppressed fixtures below go
> valgrind-clean.** Do not close this report by editing prose; close it by
> deleting `requires.no-leak-check` from `free-lift-bind`,
> `unsafe-closure-capture`, `hkt-stdlib-parser-instances`, and
> `cps-backend-fn-param` because they are actually clean. Owner instruction,
> 2026-07-21.

**Severity:** low (bounded per-closure-construction leak; not a crash or
miscompile). General codegen -- **not** CPS/effect-specific (reproduces with no
effects at all).

> **Status (2026-07-21, CURRENT) -- S1 + S2/Model U RESOLVED; Model R LANDING
> behind `--enable=closure-drop-glue`. Report STILL OPEN for the flag-off httpd
> residual.** READ THIS BEFORE RE-ANALYZING: the design questions (Model U vs
> Model R, why type-erasure blocks a static drop-glue, why a walk needs
> move/uniqueness) are SETTLED and written up in
> `docs/upcoming/closure-drop-glue-plan.md` (progress notes 2026-07-21b..g). Do not
> re-derive them. The remaining work is the concrete slices that note lists, not a
> fresh investigation.
>
> - **RESOLVED (base language, flag-off):** the `make-scaler` value-closure repro
>   (S1c) and the STORED-in-a-Turmeric-holder case (`hkt-stdlib-parser-instances` /
>   `-backtrack-instances`, closures in a `Parser` value freed by the holder's
>   generated drop glue -- S2/Model U). Eight leak-check opt-outs this leak was
>   gating are dropped and LSan-clean: `cps-backend-fn-param`, `free-lift-bind`,
>   `unsafe-closure-capture`, `hkt-stdlib-parser-instances`,
>   `hkt-stdlib-backtrack-instances`, `ascribe-fat-closure-call`,
>   `fat-closure-ascription`, `captureless-autobox`.
>
> - **Model R -- LANDED behind the `closure-drop-glue` experiment (OFF by
>   default; base language byte-for-byte unchanged, snapshots stable).** A heap fat
>   env now carries an 8-byte drop-glue header at `env[-1]` (prepend, so `fat[0]`
>   dispatch + capture-by-field are byte-identical); `TUR_CLOSURE_DROP` releases
>   ANY fat handle through it. Slices, each fixture-verified:
>     1. Header ABI foundation + `TUR_CLOSURE_DROP` + scope-exit wiring.
>     2. rc-capture walk: rc captures are retained at capture and released in the
>        drop-glue (refcount-sound, no move analysis).
>     3. Uniform header across all fat representations (`struct __env_N`,
>        `__tur_fatshim` `{shim,orig}`, poly-to-fat `{shim,fn,env}`).
>     4. **Type-honesty (a) -- the `^fat` nested-closure walk.** A `^fat` capture
>        is the erased int64 carrier in the env FIELD, but the capture BINDING keeps
>        `is_fat` (it survives the `(let [_n next])` rebind), so `cap->is_fat`
>        identifies an owned closure handle. Capturing it MOVES it
>        (`binding_mark_moved`) -> a second capture is `TUR-E0005` use-after-move,
>        so the env is sole owner and the drop-glue walk (`TUR_CLOSURE_DROP` per
>        `is_fat` capture) cannot double-free. The `(fn [next] (fn [conn] ...))`
>        middleware chain now frees whole.
>     5. **httpd teardown rewire:** `TUR_CLOSURE_DROP` is emitted unconditionally
>        (flag-off == the identical `free`; all codegen snapshots regenerated with
>        just that one preamble line), and `httpd.tur`'s `free(handler)` ->
>        `TUR_CLOSURE_DROP(hb->handler)`. Result: **`httpd-mw-log` is leak-clean
>        flag-on** -- flipped to `--enable=closure-drop-glue` via a `flags` file,
>        its `requires.no-leak-check` DROPPED. (`accept_clos` is a hand-rolled
>        header-less `{__fn,hb}` box and MUST stay a plain `free`.)
>
> - **Remaining (this report stays OPEN):**
>     - Flag-OFF, the httpd/reactor family still leaks -- Model R is opt-in, so the
>       base language is unchanged and `httpd-async-mw-compose` / the other
>       `httpd-mw-*` keep `requires.no-leak-check`.
>     - The other `httpd-mw-*` fixtures capture strdup'd CORS strings as `cstr`
>       (not walked); they need owned `String` captures before flag-on is clean.
>     - `reactor.c`'s `owns_cb` free and the CPS `__dk_reap_ptr` still bare-free a
>       (now-headered, flag-on) handle. `reactor.c` is precompiled libturi C and
>       CANNOT use the codegen `TUR_CLOSURE_DROP` macro -- it needs a runtime-API
>       change (thread a drop fn through callback registration). So flag-on is safe
>       only for SIMPLE programs (no reactor / CPS-reap / async httpd) until then.
>     - Cross-function double-ownership: the move closes same-function aliasing; a
>       caller that independently frees a handle it also passed in would still
>       double-free (not a current pattern -- flag-off such handles just leak).
>
> Next-agent guidance: continue from the plan's remaining-slice list (owned
> `String` CORS captures -> more `httpd-mw-*` flag-on; reactor/CPS free-site
> rewiring -> async httpd flag-on). Do not reopen the settled design.
>
> **Progress (2026-07-20), report STILL OPEN.** Two adjacent slices landed:
>
> 1. A *different* env leak in the same machinery: a NON-escaping closure's env
>    leaked when the enclosing `let` also bound an owning `rc`/`ref` (an unrelated
>    `default: escape` false-positive on `EX_DEFER` / `EX_RC_OF` in
>    `binding_escapes_impl`). See
>    `docs/archive/history/fat-closure-env-free-owning-sibling.md`.
> 2. **S1c inferred non-retention (INLINE args):** a capturing closure passed
>    inline to a `^fat`/fn param the callee only CALLS is now freed at scope exit
>    (`Binding.nonretain_param_mask`, inline-C-gated for soundness). See the
>    closure-drop-glue-plan progress note (2026-07-20b).
>
> 3. **S1c fresh-closure-returning CALL arg:** this report's exact repro
>    `(use-it (make-scaler 2.0))` is now ASan/LSan-clean. `make-scaler`'s call
>    result is a fresh, uniquely-owned, scalar-capture env (`returns_fresh_closure`)
>    passed to a non-retaining `^fat` param, so it is hoisted and freed at scope
>    exit. See the closure-drop-glue-plan progress note (2026-07-20c).
>
> The MINIMAL REPRO above is fixed. This report stays OPEN for the remaining
> **S2** surface it also names -- closures that are STORED (returned into a
> struct/ADT and threaded onward: parser combinators, httpd middleware) rather
> than consumed once. Those need the move/uniqueness ownership model, not the
> consumed-once free. `cps-backend-fn-param`, `free-lift-bind`,
> `unsafe-closure-capture` keep `requires.no-leak-check` (different shapes;
> `free-lift-bind`'s residual is a non-closure free-monad ADT leak).

## Summary

Constructing a closure that captures locals allocates a heap "fat" env struct
(`struct __env_N { <thunk fn ptr>; <captures...> }`) via `malloc`. When the
closure escapes (returned / stored / passed as `^fat`), that env struct is never
freed -- one leak per closure construction that escapes.

## Minimal repro (no effects)

```turmeric
(defn make-scaler [k : float] : ptr<void>
  (fn [x : float] : float (* x k)))          ; captures k -> heap env
(defn use-it [^fat h : (fn [float] #fx{} float)] : float (h 3.0))
(defn main [] : int (println (use-it (make-scaler 2.0))) 0)
```

LSan: `16 byte(s) leaked` from `make_scaler` (the `struct __env_N` malloc). The
in-tree fixture `cps-backend-fn-param` hits the same site via a `handle`, but the
effect is incidental -- the leak is the closure env.

## Root cause

`src/compiler/emit_expr.c` (~`:5511`-`:5537`): a captured closure lowers to
`struct __env_N *tmp = malloc(sizeof(struct __env_N)); tmp->__fn = ...;
tmp->cap = ...;` and the fat closure value carries `tmp`. No corresponding
`free` is emitted when the closure's lifetime ends, so an escaping (or even a
locally-dropped) fat closure leaks its env. This is the closure analogue of the
by-value owning-field leak in `docs/reported/byvalue-struct-local-owning-field-leak.md`
-- part of the owning-pointer / drop-glue lifecycle work (now ACTIVE, see the
directive above), but tracked separately here because the allocation site and
fix differ.

## Fix directions

1. Give the fat-closure env the same drop treatment as other owning heap values:
   inject a `free`/`drop!` of the env when the closure value is dropped, and a
   retain/copy when it is duplicated (an escaping closure that outlives its
   constructor transfers ownership to the callee/holder).
2. This needs the closure to participate in the RC/drop / uniqueness analysis so
   a shared closure is not double-freed. This is the active build, not a
   someday-item -- see the blocking directive above.
3. `cps-backend-fn-param`, `free-lift-bind`, `unsafe-closure-capture`,
   `hkt-stdlib-parser-instances` currently carry `requires.no-leak-check`. These
   markers are the checklist to DELETE as the fix lands -- not a stable state to
   leave in place.

## Scoped fix

A concrete, phased design lives in
[docs/upcoming/closure-drop-glue-plan.md](../upcoming/closure-drop-glue-plan.md):
an env drop-glue function (`drop_glue_env_N` -- walk owning captures, then free),
S1 a scoped free for NON-escaping closures (partial-app + non-retaining HOF arg,
no ownership tracking), and S2 move-based drop glue for ESCAPING (stored)
closures. S1 clears this leak for the non-escaping fixtures; S2 covers stored
closures (parser combinators, httpd middleware).

## Verified: why the same-scope free does not reach it (2026-07-20)

Reproduced under valgrind (16 bytes, 1 block; alloc trace `malloc <- main`).
Emitted flow for `(use-it (make-scaler 2.0))`:

```c
__auto_type __ps_157 = (make_hyscaler(2.0));                    /* returns the malloc'd env */
__auto_type __ps_158 = (use_hyit((int64_t)(intptr_t)(__ps_157))); /* consumes it, never frees */
```

Two reasons the existing conservative `let_binding_env_freeable`
(emit_expr.c:1349) cannot cover this:

1. **The env escapes its constructor.** `make-scaler`'s `(fn [x] (* x k))` is a
   direct `EX_CLOSURE`, but it is RETURNED, so `closure_binding_escapes` correctly
   forbids freeing it inside `make-scaler`. The owner is now `main` (holding it as
   `__ps_157`), which received it as a CALL RESULT, not a same-scope `EX_CLOSURE`
   literal -- so no local emit site knows it is a fresh, uniquely-owned env.

2. **`^fat` is a dispatch marker, not a linearity guarantee.** `use-it`'s
   `^fat h` selects the fat-call `{thunk,env}` protocol (elab_fns.c:1876,
   `is_fat`); it does NOT prove `h` is used exactly once and unaliased. A blanket
   "free the `^fat` param's env at scope exit" would double-free whenever the
   closure is shared (the same failure mode proven for `cps-reopen`: an
   ownership-blind free crashes).

So the fix genuinely needs the closure to carry cross-function ownership -- an
RC/drop or uniqueness bit that travels with the fat-closure value from
constructor through holder to consumer -- so exactly one owner frees the env.
That is the drop-glue subsystem the report already names; a same-scope or
per-site free cannot be made sound here. Confirmed low-severity (bounded, one
env per escaping construction; `cps-backend-fn-param` keeps `requires.no-leak-check`).
