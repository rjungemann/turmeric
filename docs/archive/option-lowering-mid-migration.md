## Compiler emits `none()` / `some()` returning `int64_t` while consumers expect `Option__T` struct

**Status:** Reported (2026-06-17). Surfaced while attempting to compile
`tur-tourist`'s `middleware` and `ctx-attrs-after` fixtures end-to-end.

**Severity:** Compiler regression. Type-checking passes; codegen emits
inconsistent C that fails the `cc` step. Affects every function that
returns `(Option T)` and constructs its result via stdlib `some` /
`none`. Looks like a partial migration from the old "Option-as-heap
`tur_option_t *`" lowering to the new "Option-as-by-value
`Option__T` struct" lowering, where call-sites and call-graph
references were not migrated in lock-step.

## Summary

A function declared `: (Option int)` (or any other `(Option A)` /
`(Option Response)`) whose body constructs the result with
`(some x)` / `(none)` emits C that *almost* works:

- The function signature uses the **new** struct lowering
  (`static Option__int my_fn(...)`).
- The local variable that holds the constructed value also uses the
  new lowering (`Option__int __t24;`).
- But the assignment `__t24 = none();` calls the *old*-style
  emitted constructor, which is declared as returning `int64_t` (a
  pointer-cast-to-int produced by `malloc(sizeof(tur_option_t))`).
- The result is `assigning to 'Option__int' (aka 'struct Option__int')
  from incompatible type 'int64_t'` at the C step.

The function then makes the symmetric mistake on return: it casts the
new-style struct **back** to `tur_option_t *` to read its
fields, which the C compiler rejects as `operand of type 'Option__int'
... where arithmetic or pointer type is required`.

## Observed

Compiler at the rebased main of `turmeric.git` (commit `e73c872f` --
the "Fix mutmap-eq by splitting storage access into helper natives"
PR -- with `2ddd402b` "Retype MutableMap API to honest (MutableMap K
V); retire its carrier" as its predecessor) emits the following for
`spices/tourist/src/tourist/param.tur`'s `ctx-attr-get`:

```c
static Option__int tourist__param__ctx_hyattr_hyget(int64_t ctx,
                                                    const char * key) {
    Option__int __t23;
    {
        int64_t cell_1003 = tourist__param___un_unctx_hyattr_hyfind(ctx, key);
        Option__int __t24;
        if ((cell_1003) == (INT64_C(0))) {
            __t24 = none();                  /* <-- int64_t -> Option__int */
        } else {
            __t24 = some(tourist__param___un_unctx_hyattr_hycell_hyval(
                          cell_1003));        /* <-- same */
        }
        __t23 = __t24;
    }
    tur_option_t *__t25 = (tur_option_t *)(intptr_t)(__t23);
        /*  ^-- cast struct value to pointer -- illegal */
    return (Option__int){.is_some = __t25->is_some, .value = __t25->value};
}
```

The Turmeric source for this is the rolled-back-to-`int` version of
`ctx-attr-get`, restored to its v0.2.0 shape after the discovery (the
report still applies; the Option-returning rewrite was the one that
triggered the codegen). A minimal repro is also achievable by writing
*any* function that returns `(Option T)` constructed from
`(if cond (some x) (none))`.

`cc` errors:

```
/tmp/tur-build/.../middleware_tur.c:4267:23: error: assigning to
  'Option__int' (aka 'struct Option__int') from incompatible type
  'int64_t' (aka 'long long')
              __t24 = none();
                      ^~~~~~

/tmp/tur-build/.../middleware_tur.c:4273:65: error: operand of type
  'Option__int' (aka 'struct Option__int') where arithmetic or pointer
  type is required
        tur_option_t *__t25 = (tur_option_t *)(intptr_t)(__t23);
                                                        ^
```

## Expected

Either:

- `none()` / `some(x)` are emitted as returning the new `Option__T`
  struct directly (consistent with the new lowering everywhere else in
  the function), **or**
- consumers continue to expect the old `tur_option_t *` heap pointer
  and the `Option__T` struct lowering is wired up identically through
  the return slot.

The current code emits both: the signature, the local, and the final
return use the new lowering; the constructors and the readback through
`__t25` use the old. They are not compatible.

## Root cause -- probable

`src/compiler/emit_module.c` ~line 2825 emits the old
`tur_option_t` builtin shell. `src/compiler/emit_core.c` ~line 2505
and `emit_expr.c` ~4294 carry comments mentioning the by-value
`Option__T` / `Result__T__U` struct lowering. `emit_fns.c` ~590 /
~1196 mention the same. There appear to be at least two code paths
that decide how to emit an Option-typed return value, and the call to
`none()` / `some()` falls through the old path while the function
signature lowering uses the new one.

Without a more careful read of the compiler I cannot pin down whether
the regression came in with the MutableMap retype (`2ddd402b`) or
shortly before -- the `2ddd402b` commit retypes `(MutableMap K V)` to
its by-value shape and is the most likely point at which the Option
lowering also got partially updated in sympathy. The earlier
`tur-tourist` v0.2.0 work was authored against the old (heap-pointer)
Option ABI and compiled cleanly at the time -- the breakage surfaced
the first time the fixtures were exercised against the rebased
compiler.

## Repro

```sh
git -C ~/Projects/turmeric checkout main
git -C ~/Projects/turmeric pull --rebase origin main
cmake --build ~/Projects/turmeric/build -j

cd ~/Projects/turmeric-spices/spices/tourist
git checkout tourist-v0.2.0-ctx-redesign
TUR_STDLIB_DIR=~/Projects/turmeric/stdlib \
  ~/Projects/turmeric/build/tur test tests/fixtures/middleware
```

Type-check passes; `cc` fails with the errors above. The same shape
reproduces with a minimal source file that just declares
`(defn f [b : bool] : (Option int) (if b (some 1) (none)))` and is
compiled with `tur build`.

## Impact

- Blocks any spice or program that returns `(Option T)` via the
  stdlib constructors. That includes the audit-S3 fix direction in
  `docs/reported/spices-int-stand-in-audit-2026-06-14.md` (the
  remaining open `param` / `capture` / `req-header` rows would also
  need `(Option T)` returns).
- The `tur-tourist` v0.2.0 redesign typed `use!` as
  `(c-fn [Ctx] (Option Response))` and added `ctx-attr-get : (Option
  int)`. Both shapes are blocked from compiling end-to-end against the
  current compiler. The signatures *type-check*; the failure is at
  `cc` time on the emitted C.
- For now the `tour-tourist` v0.2.x branch has rolled `ctx-attr-get`
  back to `: int` (with a separate `ctx-attr-has?` predicate), and
  the `middleware` and `ctx-attrs-after` fixtures still describe the
  intended `(c-fn [Ctx] (Option Response))` shape -- they will start
  compiling again once this is fixed.

## Proposed fix directions

1. **Audit every `Option__T` emitter to use the same lowering
   convention** -- either entirely heap-pointer (old) or entirely
   by-value struct (new). The latter is the documented direction
   (`emit_core.c` comments call out the "by-value sink struct
   `Option__T` lays them out at native..."), so the constructors
   `some` / `none` and the unboxing helper should be moved over.
2. **Add a regression fixture under `tests/fixtures/`** that returns
   `(Option int)` via `(if b (some x) (none))` -- exactly the shape
   above. It is currently absent; if it had been present the
   regression would have been caught when the MutableMap retype
   landed.
3. **Once fixed**, lift the rolled-back `ctx-attr-get : int` back to
   `ctx-attr-get : (Option int)` in `tur-tourist` and remove the
   `ctx-attr-has?` predicate added in the workaround.

## Cross-references

- `docs/reported/sleep-ms-not-auto-loaded.md` -- separate finding,
  surfaced by the same end-to-end-fixture run.
- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the S3
  follow-up rows (`param`/`capture`/`req-header`) are blocked on
  this; their `result<cstr>` shape hits the same lowering inconsistency
  as Option (`Result__T__U` is a sibling struct).

## Resolution (2026-06-17)

Fixed. The straddle was in the **control-form** lowering, not the
constructors themselves. The constructors (`some`/`none`) and the
function-return bridge were already consistent for a *bare* producer
body (`(defn g [] : (Option int) (some 1))` compiled fine via the
carrier->concrete return unbox). The break was that a control form
(`if`/`let`/`do`) whose tail leaves are carrier producers declared its
**result temp by value** (`Option__int __t`) while assigning the int64
carrier handle that `some()`/`none()` actually return -- so
`__t = none()` was an `int64_t`->struct mismatch and the return readback
dereferenced the struct as a `tur_option_t *`.

Three coordinated changes in the emitter, all keyed on the existing
return-side predicate (`fn_body_tail_is_carrier_producer` &&
!`fn_body_tail_emits_byvalue_carrier_abi`):

1. **`emit_expr.c` -- `emit_control_result_temp_decl`** (new helper, used
   by `emit_if_value`, `emit_let_value`, `emit_do_value`): an `if`/`let`/
   `do` result temp whose static type is a carrier-ABI aggregate but whose
   tail leaves are all carrier producers is now declared as the `int64_t`
   carrier, matching the `some()`/`none()` values flowing into it. The
   downstream consumer (the fn-return bridge) unboxes the carrier exactly
   as it already did for a bare producer body.
2. **`emit_expr.c` -- `emit_binding_repr_c_name`**: a `let` binding whose
   initialiser is such a control form is declared as the carrier too, so
   `Option__int o = <int64>` no longer mismatches; downstream uses bridge
   via the var's `emit_byvalue_carrier_abi=false` flag.
3. **`emit_core.c` -- canonical Option carrier->concrete readback** and
   **`emit_expr.c` -- through-carrier `.field` access**: both now
   NULL-guard the Option carrier, because `none` is the `0` handle.
   The return readback collapses to `(Option__int){0}` and a `.is-some`/
   `.value` access reads back `false`/`0` instead of dereferencing NULL.
   (This also fixed a **pre-existing** segfault: any function returning
   `(Option int)` whose body was a bare `(none)`, then field-accessed,
   crashed on the NULL deref -- independent of control forms.) Result has
   no NULL carrier, so it keeps the unguarded path.

Regression fixture: `tests/fixtures/option-control-form-construct/`
exercises `if`/`let`/`do` construction of `(Option int)` and
`(Result int cstr)`, the let-bound + field-accessed consumer, the
`some?` carrier consumer, and the `none` path. Snapshot churn: the
Option-carrier field-access NULL-guard touches the preloaded stdlib
`option-eq?` Eq instance, so all 77 `expected.c` snapshots were
regenerated in the same change (suite: 1655 passed, 0 failed).

### Still open (filed separately)

Direction #3 (lift `ctx-attr-get` back to `(Option int)` in
`tur-tourist`) is spice-side work, untouched here. And the *consumer*
straddle remains: `(some? (g))` / `(unwrap-or (g) d)` where `g` returns
`(Option int)` by value still fail to compile, because `some?` /
`unwrap-or` / `option-map` / `option-free` declare their parameter
`o : int` (the carrier ABI) -- a by-value `Option__int` result cannot be
passed. That is the consumer half of this same migration (and a
`No Lazy :int Stand-Ins` violation -- they should be `o : (Option A)`).
Filed as `docs/archive/option-consumers-typed-as-int-carrier.md`, since
resolved via a call-site concrete->carrier bridge (the honest `:int`
retype stays blocked on migrating the carrier-`int` Option producers).
