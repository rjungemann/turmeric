# `^fat` parameters are emitted as `void *`, warning `-Wint-conversion` in inline-C bodies

> **RESOLVED (2026-06-05).** A `^fat` parameter on an **inline-C-bodied** `defn`
> is now emitted as `int64_t` -- the canonical carrier ABI -- instead of `void *`,
> across all four C signature sites (definition, forward prototype, and both
> `__cps` wrapper sites), and the call site coerces such arguments with
> `(int64_t)(intptr_t)` rather than the old `(void *)(intptr_t)` up-cast. The
> minimal repro and the `pair-fn-arg` helper in `arrow-instance-apply` now compile
> warning-free. The fix is **scoped to inline-C bodies on purpose**: emitting
> `int64_t` for *every* `^fat` param cascades into the typeclass-dictionary
> codegen (instance methods would get `int64_t` params while the dict's
> function-pointer slots stay `void *(*)(void *)`, producing
> `-Wincompatible-pointer-types`), which is a far broader change than this
> ergonomics gap warrants. Compiler-generated bodies keep the `void *` fat
> carrier, so the fat-call dispatch and typeclass machinery are untouched. See
> **Resolution** at the end.

**One-line summary:** A `defn` parameter declared `^fat` is emitted into the C
function signature as `void *`, but every other Turmeric value crosses the C
boundary as `int64_t` -- so an inline-C body that handles the `^fat` parameter
with the idiomatic `int64_t` handle pattern (`p->slot = f;`, `int64_t h = f;`)
triggers `-Wint-conversion` ("makes integer from pointer without a cast").

**Severity:** Ergonomics gap / latent build break. Not a miscompile -- the
implicit `void *`->`int64_t` conversion is value-preserving on LP64, and the
caller already round-trips the same bits through `(void *)(intptr_t)...` -- but
(1) it would become a hard error under `-Werror`, (2) it is an ergonomic trap:
the *natural* inline-C handle idiom warns, forcing every inline-C `^fat`
consumer to hand-write `(int64_t)(intptr_t)f`, and (3) it is inconsistent with
the rest of the value ABI, where handles (including `:ptr<void>`-carried fat
closures everywhere else) are `int64_t`.

## Minimal repro

```turmeric
(defn store [^fat f] : int
  ```c int64_t h = f; return (int)h; ```)

(defn id [x : int] : int x)

(defn main [] : int
  (println (store id))
  0)
```

Build/run (`tur run /tmp/fatc.tur`):

```
.../_tmp_fatc_tur.c:4457:21: warning: initialization of 'int64_t' {aka 'long int'}
  from 'void *' makes integer from pointer without a cast [-Wint-conversion]
```

The emitted C:

```c
static int64_t store(void * f) {          /* <- ^fat param typed void * */
    int64_t h = f; return (int)h;         /* <- warns: void * -> int64_t */
}
...
printf("%lld\n", (long long)(store((void *)(intptr_t)(__t26))));
                                  /* ^ caller pre-casts an int64_t handle back to void * */
```

**Observed:** the `^fat` param is `void *`; assigning it to an `int64_t`
(the idiomatic handle representation, and what the caller started from) warns.
**Expected:** an inline-C body can treat a `^fat` handle as `int64_t` -- the
same as every other Turmeric value -- without a manual cast and without a
warning.

This is also the (benign) warning observed while validating the resolved
`arrow-instance-apply` fixture, whose `pair-fn-arg` helper does
`p->e1 = f;` with a `^fat` `f` and an `int64_t` slot:

```
.../tests_fixtures_arrow-instance-apply_input_tur.c:5207:9: warning:
  assignment to 'int64_t' from 'void *' makes integer from pointer without a cast
   5207 |   p->e1 = f; p->e2 = arg; return (int64_t)(intptr_t)p;
```

## Root cause

`src/compiler/emit_fns.c`, the parameter-signature loop (`emit_fns.c:436-481`).
A `^fat` parameter carries `param_types[i] == :ptr<void>`, and the `else`
branch emits its C type via `emit_type_c_name(ctx, param_ty)`
(`emit_fns.c:468`), which renders `:ptr<void>` as `void *`. The same loop emits
the CPS wrapper signature (`emit_fns.c:678-682`), so the wrapper path has the
identical shape.

Nothing downstream needs the `void *` spelling: the caller already emits the
argument as `(void *)(intptr_t)<int64_t-handle>` (i.e. it *starts* from an
`int64_t` handle and casts up to `void *` only to match this signature), so the
`void *` choice is pure friction at the inline-C boundary -- the one place a
human writes the body and naturally reaches for `int64_t`.

The existing comment at `emit_fns.c:454-460`
(referencing `bare-fat-sink-poly-box-slot0-int64-mismatch.md`) already
establishes that a `^fat` param is "a fat-closure *carrier* handle, never a
by-value fn"; a carrier handle's canonical C type is `int64_t`, not `void *`.

## Proposed fix directions

1. **Emit `^fat` params as `int64_t`.** In the `emit_fns.c` param loop, special-
   case `fd->params[i]->is_fat` to emit `int64_t` (mirroring the `TY_FN`
   branch at `emit_fns.c:441-444`), in both the direct signature and the
   `__cps` wrapper signature. Then drop the now-redundant `(void *)(intptr_t)`
   up-cast at the call site (it already lowers from an `int64_t` source), or
   leave it harmless as `(int64_t)(intptr_t)`. This makes the idiomatic
   inline-C body (`p->e1 = f;`, `int64_t h = f;`) warning-clean with no caller
   change required if the cast is simply normalized to `int64_t`.

2. **If `void *` must stay** (e.g. some path relies on pointer typing): cast at
   the *use* inside generated inline-C splicing is not possible (the body is
   user-authored), so the only alternative is to document the requirement and
   have inline-C authors write `(int64_t)(intptr_t)f`. This is strictly worse
   than #1 and is recommended only if #1 surfaces a real dependency on the
   pointer type.

## Validation of a fix

- The minimal repro above compiles with **no** `-Wint-conversion` warning and
  prints the handle round-tripped through inline-C.
- `tests/fixtures/arrow-instance-apply` still prints `42 / 42 / 1007` and its
  generated C no longer warns on the `pair-fn-arg` slot assignment.
- The full suite stays green (`bash tests/run.sh`, zero `FAIL`), and the
  bare-fat fixtures referenced by `emit_fns.c:454-460`
  (`bare-fat-sink-poly-box-slot0-int64-mismatch`) are unchanged.
- Worth adding a tiny regression fixture: a `^fat`-param `defn` with an
  inline-C body that stores `f` into an `int64_t` slot, snapshotting the
  warning-free `expected.c`.

## Resolution

Implemented fix direction #1, **scoped to inline-C bodies** (`body_is_inline_c`).
The broad "every `^fat` param" form was tried first and rejected: it flips the
arrow typeclass instance methods' params to `int64_t` while the generated
`dict_Arrow_T` slot types stay `void *(*)(void *)`, raising
`-Wincompatible-pointer-types` plus a `return f;` int->ptr warning in each
instance method. Narrowing to inline-C bodies fixes exactly the reported cases
(hand-written C that treats the carrier as a handle) and leaves the fat-call
dispatch and typeclass-dictionary machinery -- which both rely on the `void *`
carrier -- completely untouched.

Changes:

1. **Signature emission (4 sites).** A `^fat` param on a `defn` whose body is
   `EX_INLINE_C` is emitted as `int64_t`:
   - direct definition signature (`src/compiler/emit_fns.c`, param loop),
   - `__cps` wrapper signature (`emit_fns.c`),
   - forward prototype (`src/compiler/emit_module.c`),
   - `__cps` wrapper forward prototype (`emit_module.c`).

   All four gate on `is_fat && body-is-inline-C`, so prototype and definition
   always agree. (Inline-C bodies are never CPS, so the two `__cps` gates are
   inert in practice; they are kept for parity.)
2. **Call-site coercion.** When the callee binding `body_is_inline_c` and the
   target param is `^fat` (`arg_fat[param_idx]`), the argument is coerced with
   `(int64_t)(intptr_t)` instead of the `(void *)(intptr_t)` up-cast
   (`src/compiler/emit_expr.c`). Non-inline-C `^fat` callees are unchanged.

Why this is safe for existing inline-C bodies: the canonical fat-apply idiom
`TUR_APPLY1(f, x)` expands to `(... (void *)(intptr_t)(f) ...)`, i.e. it already
coerces the carrier through `intptr_t`, so it is agnostic to whether `f` is
spelled `void *` or `int64_t`. Only bodies that assign the carrier *directly*
into an `int64_t` (e.g. `pair-fn-arg`'s `p->e1 = f;`) were warning, and those
are exactly the ones this fix makes clean.

**Validation.**
- The minimal repro and `tests/fixtures/arrow-instance-apply` (`42 / 42 / 1007`)
  compile with no `-Wint-conversion` / `-Wincompatible-pointer-types` warning;
  the prototype, definition, and every call site emit `int64_t` consistently.
- Codegen snapshots regenerated: 127 `expected.c` files updated (the stdlib
  prelude carries the affected inline-C `^fat` helpers -- `option-map`,
  `map-eq-*`, `vec-eq?`, `set-eq-cmp?`, `result-eq?`, `pair-eq-carrier?`, etc.).
  Every changed line is purely a `void *`<->`int64_t` flip; no other churn.
- Full suite green: `summary: 1487 passed, 0 failed`, zero `FAIL`, zero codegen
  mismatch.
