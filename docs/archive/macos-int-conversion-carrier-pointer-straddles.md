# Residual carrier<->pointer straddles are hard errors on modern toolchains

**RESOLVED 2026-08-01** (arm64 macOS, Apple clang 21, macOS 27, at `54fef281d`).
All four open fixtures build clean and match their expected output; the whole
corpus is `2502 passed, 0 failed` on a toolchain where `-Wint-conversion` is a
hard error, and the JIT corpus is `2416 passed, 0 failed, 48 skipped`. See
**Resolution** below.

Both fix levels the report named are done: the "low risk" one that turns the
four fixtures green, and -- in a second pass -- the "proper" one, which turned
out to be *narrower and sharper than described*, and which uncovered a silent
name collision no compiler diagnoses. See
[The "proper" fix, measured](#the-proper-fix-measured-2026-08-01).

The one thing this report carried that was never a straddle --
`data-literal-nested` -- was split out, and is now also resolved:
[`vec-empty-like-monomorph-selects-int-element`](vec-empty-like-monomorph-selects-int-element.md).

---

**Severity was: medium (build breakage on any sufficiently new toolchain, five
fixtures).** The affected programs failed to compile at the `cc` step. They
built fine on the Linux CI leg, so the suite was green there and this was
invisible to the release gate.

Found 2026-07-30 on arm64 macOS (Apple clang 21.0.0, macOS 27.0) while checking
`claude/j0-jit-engine-plan-znqibo` for regressions after
`66c3bb7c4` (merge of `origin/main` into the JIT engine branch).

**Confirmed 2026-07-31 on Windows** (MSYS2/UCRT64, gcc 16.1.0, main at
`f630230e5`) during a Windows-support sweep. This is NOT a macOS/clang quirk:
it is "any toolchain new enough to promote the diagnostic."

## Why Linux CI could not catch this

`-Wint-conversion` (assigning/passing an `int64_t` where a pointer is expected,
or the reverse) has been an **error by default** since clang 15, and Apple clang
21 enforces it. GCC promoted `-Wint-conversion` and
`-Wincompatible-pointer-types` to errors in GCC 14. The gcc on the CI Linux leg
is older than that, so every one of these emitted a warning there and compiled
clean. This class has bitten before -- see the "Apple clang 17
`-Werror=int-conversion`" entry in `CHANGELOG.md`.

`.github/workflows/ci.yml:30` runs a `macos-latest` leg, and it was red on
`main` on exactly these four from run 2202 (job 91324836416, head `8b1ea4380`)
onward:

```
summary: 2495 passed, 4 failed
  - conv-defstruct-option-fn-element (build failed)
  - defalias-composite (build failed)
  - fn-value-matrix-ok-rows (build failed)
  - hkt-ap-fn-in-container (build failed)
```

Note that the two `-Wno-error=` downgrades that used to hide this were
deliberately removed (`src/main.c:5231-5241`). Do not re-add them; the removal
is correct and these were the genuine remaining straddles it exposed.

## Repro (historical)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF
cmake --build build -j
./build/tur build tests/fixtures/hkt-ap-fn-in-container/input.tur
```

(`-DTUR_DEBUG_SANITIZE=OFF` avoids the unrelated macOS ASan startup deadlock
documented in `CLAUDE.md`.) Off a promoting toolchain the check was
`./build/tur build tests/fixtures/<name>/input.tur 2>&1 | grep int-conversion`.

## Case A -- monomorphized ctor carrier field

`conv-defstruct-option-fn-element`, `hkt-ap-fn-in-container`.

```
error: incompatible pointer to integer conversion passing 'void *'
       to parameter of type 'int64_t'
  tur_adt_Option__fn1_float__float __ps_213 =
      (ctor_Option__fn1_float__float(true, x));
```

An ABI-specialized clone spells its monomorphized fn-typed parameter `void *`
(`some__spec__...Option__fn1_float__float_void__(void * x)`), but the ctor slot
it feeds is the int64 carrier.

### Why every earlier attempt fell through

The root cause is that **the ctor's field slot C type cannot be re-derived from
the field's Type.** `type_c_name` for `TY_FN` (`types.c:3147-3175`) branches on
`cfnptr`/`boxed`, which carry no type identity, and `append_type_mangle` for
`TY_FN` (`types.c:907-915`) mangles only arity + arg kinds + result kind --
**not** `boxed`/`cfnptr` -- with `type_eq` comparing the same triple. So the two
`Option` monomorphs' payload slots genuinely differ:

```c
static tur_adt_Option__fn1_int__int   ctor_Option__fn1_int__int  (bool, void *);
static tur_adt_Option__fn1_float__float ctor_Option__fn1_float__float(bool, int64_t);
```

while racing for one typedef. Every cast in the ctor-argument block of
`emit_value_dispatch` re-derives the slot type and so collapses the two: a
blanket int64 cast fixes the float monomorph and breaks the int one (this was
observed directly -- fixing case A's first error moved it to the mirror-image
error on the sibling monomorph). Three separate attempts died here, including
one routed through `adt_field_type_for_app`, which resolves correctly but whose
consumer bottoms out in the same `type_c_name`.

## Case B -- return-site straddle

`defalias-composite`:

```
error: incompatible integer to pointer conversion returning 'int64_t'
       from a function with result type 'tur_adt_Cons__int *'
  return cons(..., ...);
```

`fn-value-matrix-ok-rows` is the same shape in the other direction (`return v;`
and `return __env___env_1376->c;` from a `void *`-returning function).

The return-site bridge at `src/compiler/emit_fns.c:4264-4281` existed; the three
sites fell through two gaps in its guard:

- `fn-value-matrix-ok-rows`: `emit_fns.c:4266` explicitly excluded `void *`
  return types.
- `defalias-composite`: the value is a bare carrier-ctor *call* (`cons(...)`),
  which is neither `(int64_t)`-prefixed nor a bare recorded ident, so neither
  disjunct at `:4268-4271` matched.

An earlier attempt removed the `void *` exclusion alone; it fixed nothing,
because all three sites also fail the guard's *value*-side test -- a parameter,
a closure-env field read and a call expression are all absent from the localvar
table. It was correctly backed out, along with a recorded warning **not** to
register parameters in that table (it is keyed by bare C name and reset per
PROGRAM, so one function's `v` would define the type every other function's `v`
resolves to).

---

## Resolution (2026-08-01)

Both cases were fixed by the same principle the earlier attempts kept missing:
**stop re-deriving an emitted C type from a Type, and consult ground truth.**

### Case B -- ask the typed AST, not the emitted string

`fn_tail_emits_int64_carrier` (`src/compiler/emit_fns.c`) decides from the AST
whether the tail expression is emitted as the int64 carrier, which the string
sniff structurally cannot:

- `EX_VAR` -- a fn-typed binding is carried as the int64 fn-ABI carrier both as
  a parameter (the `TY_FN` param branch, `emit_fns.c:3416`) and as a
  closure-env field (`emit_expr.c` pins `TY_FN` captures to `int64_t`). Two
  spellings opt out and keep a concrete C type -- a rank-2 poly param
  (`tur_poly_fn_t`) and a `cfnptr` -- so both are excluded.
- `EX_BUILTIN` -- a `BS_FUNC_CALL` builtin whose declared result is `TY_INT`
  emits a call to a preamble helper returning `int64_t` (`cons` is
  `builtins.c:152`).

The `void *` exclusion in the guard is dropped **only** for a tail this
predicate recognizes; the string-sniff disjuncts keep it, so nothing that used
to be left alone starts getting cast.

This is the "consult the typed AST" fix the backed-out attempt's note asked for,
and it needed neither a function-scoped localvar table nor any parameter
registration.

### Case A -- record the ctor's real param C type at registration

`emit_sig_record_param_ctype` / `emit_sig_lookup_param_ctype` already existed
alongside the return-type recording. `record_adt_app_ctor_sigs` (`types.c`) now
records each ctor param's C type from the same `adt_field_c_type(def, fld, args)`
call `emit_registered_adt_app_rec` renders the prototype from, and a final
normalization pass in the ctor-argument block of `emit_value_dispatch`
(`emit_expr.c`) bridges the arg to the recorded slot type.

It fires **only** on an int64<->pointer straddle, and only when the argument's
own emitted C type is known for certain: an explicit `(T)(intptr_t)` cast the
block already applied, or a bare reference to a parameter of the active ABI spec
(resolved through `emit_var_spec_arg_type`, whose result is by construction the
clone signature's C type). A pointer->pointer mismatch is deliberately left
alone -- that means a mis-selected monomorph, and a cast would paper it over.

One prerequisite made this work where the earlier attempt had not. Recording at
registration time was recording the **wrong** type: `type_register_adt_app`
called `record_adt_app_ctor_sigs` on the *incoming* `t` before the dedup lookup,
and since `type_eq` ignores `boxed`/`cfnptr`, an equal-but-differently-boxed `t`
would file `int64_t` against a slot the emission renders `void *`. Recording now
happens off the **canonical** `g_adt_apps[i].type`, so the recorded string is
always the one the prototype will carry.

This is the report's own "low risk" fix level. For the "proper" level, and what
it actually turned out to be, see the next section.

## The "proper" fix, measured (2026-08-01)

The report proposed a second level: put `boxed`/`cfnptr` into the `TY_FN` mangle
**and** `type_eq`, "higher blast radius (corpus-wide symbol renames, large
snapshot churn) but it removes the latent silent-layout merge, which is the
actual defect."

Taken literally that is **wrong in one half and unnecessary in the other**. What
the measurement found instead:

### `type_eq` must NOT change -- it already does the right thing

`type_eq` is not structural equality; it is an interchangeability relation with
two deliberate rules for `TY_FN`:

- Its very first statement (`types.c:90-103`) equates a **boxed** `TY_FN` with
  `TY_PTR_VOID`, so closures flow through legacy `:ptr<void>` sinks with no
  coercion node. Load-bearing back-compat.
- `types.c:118-122` already refuses to equate a **cfnptr** with a **boxed** fn,
  "what keeps a fat closure from silently flowing into a raw C callback sink."

So `type_eq` already draws the one distinction that matters, and adding `boxed`
to it would contradict its own opening rule. No change was made there. Verified
empirically: a bare and a boxed `(fn [int] int)` in one `Option` share a single
monomorph, and that is **correct** -- they are type_eq-equal, both 8 bytes, same
bits, and the payload slot is `int64_t` either way.

### The mangle DID have a real hole, and it was the cfnptr one

The mangle's stated contract is "mangle exactly what `type_eq` compares." It
covered arity + arg kinds + result kind -- but not the cfnptr-vs-boxed rule. So
two types the checker holds **distinct** collided on one name:

```c
/* one program, both emitted, both under the SAME guard */
#ifndef TUR_FN_tur_adt_Option__fn1_int__int
static ... ctor_Option__fn1_int__int(bool _0, void * _1)                    { ... }
#ifndef TUR_FN_tur_adt_Option__fn1_int__int          /* <- preprocessed away */
static ... ctor_Option__fn1_int__int(bool _0, tur_fnptr_int64_t_int64_t_t _1) { ... }
```

Two registry entries, one C name, the second definition silently dropped, and
the `(c-fn ...)` view left using the closure view's `void *` slot. **No
diagnostic from any compiler, on any platform, at any warning level** -- unlike
every other symptom in this report, which at least announced itself on a
promoting toolchain. Benign only because every `TY_FN` representation happens to
be a same-bits 8-byte word; a variant that is not would be a silent miscompile.

Fix: one token, `if (t.as.fn.cfnptr) buf_putc(b, 'c')`. `boxed` is deliberately
NOT mangled -- boxed and bare share one registry entry, so mangling it would
only make that entry's name depend on registration order.

The feared "corpus-wide symbol renames, large snapshot churn" did not
materialize, because only `cfnptr` moved and almost nothing uses a `(c-fn ...)`
as a container element: **4 snapshots, 8 lines, all of it one typedef changing
position.**

### It exposed a latent ordering bug

With the names split, the `(c-fn ...)` monomorph is emitted for the first time
-- and it names `tur_fnptr_int64_t_int64_t_t` in its own typedef and ctor, while
the final assembly wrote fn-ptr typedefs *after* the ADT monomorphs. That was a
dangling reference the `#ifndef` collision had been hiding.

The two orders are both load-bearing and now differ on purpose
(`emit_module.c`): **generation** must run `adt_apps` first, because emitting a
monomorph is what *registers* the fn-ptr typedefs via `type_c_name`; **output**
must put the typedefs first, because a monomorph over a cfnptr element
references one.

### Coverage

- `tests/check-monomorph-name-collision.sh` gains the `cfnptr-vs-boxed` repro.
  Verified to fail loudly on a revert, on **both** of the guard's properties
  (A: one guard, two different bodies; B: the expected second name absent).
- `tests/fixtures/cfnptr-vs-boxed-monomorph-split` pins that both views still
  run -- `11` through a bare code pointer, `42` through the fat protocol.

The two views must never meet at a call site in either repro: the checker
rejects that on its own with TUR-E0001, so a crossing repro would never reach
codegen. That is also why the merge survived so long -- the only way to reach it
is two separate functions.

### Validation

| Check | Result |
| --- | --- |
| The four fixtures, `tur build` on Apple clang 21 | build clean, stdout matches `expected.stdout` |
| `bash tests/run.sh` (macOS arm64) | `2502 passed, 0 failed` |
| `tur run regen-snapshots -- --check` | `140 up to date` (4 regenerated for the cfnptr ordering fix; see below) |
| `tests/run-jit.sh`, Debug+JIT+ASan (CI's config) | `2416 passed, 0 failed, 48 skipped` |
| `tests/check-monomorph-name-collision.sh` | all checks passed, incl. the new `cfnptr-vs-boxed` repro |
| `tests/check-typekind-mangle-exhaustive.sh` | all checks passed |

The two bridge fixes caused **zero** snapshot drift -- both are strictly
additive branches firing only on shapes that previously emitted invalid C, so no
already-valid emission moved. The 4 regenerated snapshots come from the separate
cfnptr mangle/ordering fix below, and are one typedef changing position.

`tools/check_crossing_routing.py` counts one new `emit_var_spec_arg_type` call
site; the registry and the audit table in
`docs/archive/history/carrier-crossing-recovery-routing-plan.md` were updated in
the same change.

### What this closes

[`ci-macos-suites-fail-while-linux-passes`](ci-macos-suites-fail-while-linux-passes.md)
-- this was the sole remaining blocker on its AOT half, so
`Test (macos-latest)` goes green with it.

### What this does NOT close

`data-literal-nested` was listed in this report's Windows section as a fifth
member of the class. It is not a straddle at all but a wrong-monomorph
selection bug (`vec_empty_like__`'s `Map` monomorph calls the `int`-element
`vec_new` spec), and it is deliberately untouched -- the new bridge does not
fire on pointer->pointer. Re-filed standalone as
[`vec-empty-like-monomorph-selects-int-element`](vec-empty-like-monomorph-selects-int-element.md).

Its "the clang-still-warns half of that story is inferred, not measured -- worth
one check on a macOS box" note is now measured: Apple clang 21 warns
(`-Wincompatible-pointer-types`, not promoted to an error the way
`-Wint-conversion` was) and the fixture builds and passes.

## Guide upkeep -- done

The open-cells row in
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
moved to the closed-cells table with a resolution note in the same change.
