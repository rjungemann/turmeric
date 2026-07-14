# Closure env drop glue -- freeing captured fat-closure environments

**Status:** scoping / design (no code landed). Prepared from
`docs/reported/escaping-fat-closure-env-leak.md` and the B2 residuals in
`cps-runtime-finish-plan.md` (Progress-log PD).

**One-line:** give a captured ("fat") closure's heap env struct a real lifecycle
-- freed when the closure dies, dropped-through when stored, walk-glued when its
captures are themselves owning -- so escaping and HOF-passed closures stop
leaking and the two remaining B2 fixtures (`currying-effect-partial`,
`hkt-stdlib-parser-instances`) CPS-emit instead of evicting on `EX_CLOSURE`.

## What this unblocks

- **The escaping-fat-closure-env leak** (`docs/reported/escaping-fat-closure-env-leak.md`):
  one `malloc`'d `struct __env_N` leaked per capturing-closure construction that
  escapes (returned / stored / passed `^fat`). Currently carries
  `requires.no-leak-check` on `cps-backend-fn-param`, `free-lift-bind`,
  `unsafe-closure-capture`.
- **B2 residuals (2)** in the CPS backend: `currying-effect-partial` (a
  partial-application closure `add10 = (log-add 10)` called in a `Log` handle
  body) and `hkt-stdlib-parser-instances` (closures stored in `Parser` values).
  Both evict on `EX_CLOSURE` today because the closure cannot be admitted without
  a free.
- **The httpd middleware family** (`httpd-async-mw-attr` / `-mw-compose`) and any
  code that stores middleware/handler closures in a chain.

## Current state

A capturing closure lowers to (`emit_expr.c` ~5785):

```c
struct __env_N { int64_t __fn; <captures...> };
struct __env_N *tmp = malloc(sizeof(struct __env_N));
tmp->__fn = <thunk>; tmp->cap0 = ...; ...
```

The fat value carries `tmp` (a one-word env pointer, or a 2-word `tur_poly_fn_t`
for the rank-2 poly protocol). The ONLY free that exists today is
`let_binding_env_freeable` (`emit_expr.c:1267`): a let-bound closure is freed at
scope exit iff it is an **`EX_CLOSURE` literal**, returns a **scalar**, and
**provably does not escape** (`closure_binding_escapes`, conservative -- only ever
greenlights a free). Everything outside that narrow gate leaks:

- a **partial-application** closure (`(log-add 10)` -- init is a CALL, not an
  `EX_CLOSURE` literal),
- a closure passed as a **HOF argument** (`(free-run (fn ...) ...)` -- the arg is
  conservatively flagged escaping),
- a **stored / returned** closure (parser combinators, httpd middleware -- it
  genuinely escapes).

The CPS backend inherits this: it can only admit a capturing closure it can
free, so the un-freeable shapes evict on `EX_CLOSURE`.

## Two sub-problems (different fixes)

The residuals split cleanly by whether the closure ESCAPES its constructor:

**S1. NON-escaping closures that just aren't freed yet.** `currying-effect-partial`
(`add10` called once, locally), the `free-run` HOF args (`free-run` calls the
closure and discards it). These have a single owner and a clear scope-exit death
point; they leak only because the current gate is too narrow (`EX_CLOSURE`-literal
+ scalar-return + a conservative escape check that flags any call argument). Fix
is a scoped free, no ownership tracking.

**S2. ESCAPING closures.** `hkt-stdlib-parser-instances` (the closure is stored in
a `Parser` value that is returned / threaded), httpd middleware (stored in a
chain). Ownership transfers to the holder; the holder must drop it, and a closure
stored in two places must not double-free. Fix needs the closure to participate in
the drop / uniqueness system.

## Design

### The env drop-glue function (shared by S1 + S2)

Emit, per env type that needs it, a `drop_glue_env_N(void *p)`:

```c
static void drop_glue_env_N(void *p) {
    struct __env_N *e = (struct __env_N *)p;
    /* walk-glue: drop each OWNING capture in reverse order, mirroring the
     * ADT/struct drop-glue (emit_module.c emit_adt_byval_drop_glue). */
    <drop e->capK for each owning capture>   /* rc_strong_decrement / drop_glue_* / free */
    free(e);
}
```

- A **scalar-only** env (captures are all Copy scalars) needs no walk -- the glue
  is a bare `free(e)`; the current `let_binding_env_freeable` already emits that
  inline. The glue function matters when captures are themselves owning (an `rc`,
  a `ref`, a NESTED closure -- an env-in-env), exactly the case that leaks worst
  today.
- Reuse the existing owning-value drop machinery keyed off each capture's type
  (`needs_drop_glue`, `rc_strong_decrement`, `drop_glue_<adt>`), so a closure that
  captures an `rc<Foo>` decrements it, and a closure that captures another closure
  recurses into `drop_glue_env_M`.

### S1 -- scoped free for non-escaping closures (bounded, land first)

1. **Widen `let_binding_env_freeable`**: admit a partial-application closure (init
   is an `EX_CALL` whose `returns_closure_fn_binding` is set -- a curried under-
   saturation producing a closure) and drop the scalar-result restriction where
   the closure result cannot alias the env (needs the walk-glue so a non-scalar
   capture is dropped, not just the env freed).
2. **A HOF-arg free**: a closure passed as a call argument whose callee does NOT
   retain it (`free-run` calls-and-discards) can be freed after the call returns.
   This needs a callee "does not retain fn-param" property -- start conservative:
   a `^fat`/`(fn ...)` param that the callee only CALLS (never stores/returns) is
   non-retaining. `free-run`'s inline-C calls the interp once and returns an int
   -- non-retaining. Emit the env free after the call.
3. **CPS interaction**: the CPS backend already has the boundary-reap mechanism
   (`__dk_reap_ptr`, P3.c/P3.d). A non-escaping closure admitted on the CPS
   delegation path registers its env (and, via the glue, its owning captures) for
   reap at the entry boundary -- the analogue of `cps_closure_env_freeable`
   (which today handles only the scalar-capture let case). Wire the glue so the
   reap drops captures too.

S1 alone clears `currying-effect-partial`, `free-lift-bind`, `unsafe-closure-capture`
(dropping their `requires.no-leak-check`) without any ownership-tracking.

### S2 -- drop glue for escaping closures (the real feature)

An escaping closure is an owning heap value whose owner is the value it is stored
in (a struct field, an ADT payload, a return value). Two sound models:

- **Model U (uniqueness / move) -- preferred for STORED closures.** Plug the
  closure into the existing affine/move system (`is_moved`, `is_linear_consumed`,
  `is_affine`, `CK_MOVE`, the alias-state UT1 machinery). Storing a closure in a
  struct MOVES it (the source binding is consumed); the holding struct's drop
  glue (`needs_drop_glue`) calls `drop_glue_env_N` on the field. No refcount, no
  per-closure overhead; a double-store is a move-check error (as it already is for
  other affine values). This is how `hkt-stdlib-parser-instances` (closure stored
  in a `Parser`) and httpd middleware (closure stored in a chain node) should
  work -- the `Parser` / chain-node drop glue owns the closure.
- **Model R (refcount) -- fallback for genuinely SHARED closures.** Add a
  refcount word to the env (`struct __env_N { int64_t __rc; int64_t __fn; ... }`);
  a clone/dup increments, a drop decrements and runs `drop_glue_env_N` at zero.
  Uniform and sharing-safe, but adds a word + rc ops to every fat closure and an
  ABI change to the fat-closure protocol (the `^fat` layout, the HKT thunk
  recovery, `tur_poly_fn_t`). Reserve for closures the uniqueness model rejects
  (a closure legitimately shared by two owners).

Recommendation: land **Model U** for the stored-closure cases (covers the corpus
residuals) and only reach for **Model R** if a shared-closure fixture appears --
the ABI cost of R is high and the corpus does not yet need it.

## Phasing

- **Phase 1 (S1, bounded):** env drop-glue function + widen the scoped free to
  partial-app and non-retaining-HOF-arg closures + CPS boundary-reap wiring for
  owning captures. Clears `currying-effect-partial` and the two PD leak fixtures.
  No ABI change, no ownership tracking. **Start here.**
- **Phase 2 (S2 / Model U):** closures participate in the move system; struct/ADT
  drop glue drops closure-typed fields via `drop_glue_env_N`. Clears
  `hkt-stdlib-parser-instances` and the httpd middleware family.
- **Phase 3 (S2 / Model R, only if needed):** refcounted env for genuinely shared
  closures. ABI change; deferred until a fixture demands it.

## Risks / open questions

- **Double-free** is the cardinal risk. The escape analysis
  (`closure_binding_escapes`) is conservative (only greenlights a free), which is
  the right posture -- extend it carefully; a false "does not escape" frees a live
  env. Model U's move-check is the structural guard for S2.
- **Non-retaining callee property (S1.2):** deciding a callee does not retain its
  fn-param. Start with the syntactic "only calls it" rule (covers `free-run`);
  a general effect/escape signature on fn-params is a larger analysis -- keep it
  out of Phase 1.
- **Walk-glue ordering / cycles:** a closure that captures itself (letrec self-
  capture, already handled specially at construction -- `emit_expr.c` "Edge 1")
  must not recurse infinitely in the glue; mirror the ADT walk-glue's
  cycle-awareness or exclude self-captures from the drop walk.
- **Fat-closure ABI (Model R only):** adding an `__rc` word changes the `^fat`
  layout, HKT thunk recovery, and `tur_poly_fn_t`. Audited in
  `docs/archive/fat-closure-abi-audit-plan.md` -- coordinate there if R is ever
  needed.
- **`tur_poly_fn_t` (2-word) vs one-word env:** the drop must free the right
  object for both the plain env-pointer closures and the rank-2 poly-fat
  closures; confirm which allocation each frees.

## Test targets & exit gate

- `currying-effect-partial`, `hkt-stdlib-parser-instances` flip from
  `BODY-UNSUPPORTED` (`EX_CLOSURE`) to CPS-emitted (direct == cps == turi).
- `free-lift-bind`, `unsafe-closure-capture`, `cps-backend-fn-param` become
  ASan-clean and DROP their `requires.no-leak-check` markers.
- The minimal no-effects repro in `escaping-fat-closure-env-leak.md`
  (`make-scaler`) is ASan-clean.
- httpd middleware fixtures stay green and leak-clean.
- Full `bash tests/run.sh` green; the report moves to `docs/archive/` when closed.
