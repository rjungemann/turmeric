# M5 D.4 for the EX_ASCRIBE producer bridge is blocked on redirect coverage

> **Status:** RESOLVED 2026-06-15 (same session). Direction 1 taken: extended
> the Option C redirect (Gap 1 fix), migrated the two pins off the `(:: x :int)`
> idiom, and deleted the EX_ASCRIBE `CK_CONCRETE -> CK_CARRIER` producer bridge.
> Suite green 1636/0, zero snapshot drift. See "Resolution" at the bottom.
> Filed while looking into the remaining D.4 work after the M4c Path A branch
> was deleted (commit `f134708`).
> **Severity:** low-medium. Not a miscompile -- the bridge was correct and the
> suite was green throughout. This was a *planning correction*: the audit / M5
> retirement doc framed "delete the EX_ASCRIBE `CK_CONCRETE -> CK_CARRIER`
> branch" as a mechanical follow-up gated only on "retiring the two pin
> fixtures". It was not mechanical; it needed a redirect-coverage extension
> first (Gap 1). Gap 2 (below) turned out NOT to block: it was an artifact of
> the `(:: x (Vec A))` carrier-helper form, which the chosen bare-idiom
> migration does not use.

## One-line

The EX_ASCRIBE producer bridge at `src/compiler/emit_expr.c` (the
`CK_CONCRETE -> CK_CARRIER` site that spills a by-value `Vec__int` spec param
to the int64 carrier for `(:: x :int)`) cannot be deleted by migrating its two
pin fixtures to the Option C redirect idiom, because the redirect does not fire
for instance-method spec bodies in the configuration the pins exercise.

## Background

After Option C (the by-value twin redirect, `emit_abi_try_byval_twin_redirect`
in `emit_module.c`) landed (#364), and after deleting the now-dead M4c Path A
bridge branch (`f134708`, M5 D.4 step 1), the only remaining
`CK_CONCRETE -> CK_CARRIER` producers are the two EX_ASCRIBE bridge-pin
fixtures:

- `tests/fixtures/m5-instance-spec-constraint-var`
- `tests/fixtures/m5-spec-body-ascription-bridge`

An `emit-c` sweep over the full fixture suite (instrumented probe at the
EX_ASCRIBE `CK_CONCRETE -> CK_CARRIER` site) confirms these are the **only**
two producers -- no real stdlib/spice code reaches this site anymore.

Both pins use the explicit idiom `(vec-len (:: x :int))` / `(vec-get (:: x :int) i)`:
they widen a by-value `Vec__int` spec param to the int64 carrier so the
carrier-typed inline-C helper (`vec-len` / `vec-get`, declared `[v : int]`)
accepts it. The `(:: x :int)` ascription is exactly what fires the EX_ASCRIBE
bridge.

The plan's premise was: now that Option C exists, the carrier helper call can
be expressed as `(vec-len x)` (no ascription) and the redirect retargets it to
the `vec-len-byval` twin -- so the pins become redundant with
`m5-option-c-vec-byval-redirect`, can be retired, and the bridge deleted.

## What actually happens

Migrating a pin to the redirect idiom does **not** work. Two distinct gaps:

### Gap 1 -- redirect doesn't fire on the element-erased instance-method receiver

Rewriting `(vec-len (:: x :int))` to `(vec-len x)` inside the `MyEq [Vec]`
instance method's by-value spec
(`__inst_MyEq_myeq_qu_Vec__spec__bool_Vec__int_Vec__int`):

```c
// emitted: carrier base called with a by-value struct -> cc type error
return callee__spec__...(x, y, INT64_C(0), vec_hylen(x));
//                                          ^^^^^^^^^^^^ vec_hylen(int64_t), x is Vec__int
```

The redirect's entry probe shows it is *entered* for this `vec-len` call with
`current_abi_specialization = __inst_MyEq_myeq_qu_Vec__spec__...`, passes the
O(1) gates, finds the `vec-len-byval` twin -- then bails at the struct-app
extraction:

```
[redirect] vec-len resolved_recv.kind=18 extract=0
```

`kind=18` is `TY_STRUCT`: inside the instance-method spec body the receiver `x`
resolves to the **element-erased** bare `TY_STRUCT Vec` (the unparameterized
struct def), not the parameterized `(Vec int)` / `TY_APP`. So
`type_extract_struct_app` fails, the redirect returns false, and the call falls
through to the carrier base `vec_hylen` -- which a by-value `Vec__int` cannot be
passed to.

This is the same root issue as the (resolved-for-MutableMap) report
`docs/archive/history/m5-multiparam-instance-unconstrained-tyvar-blocks-byval-spec.md`
and the session-2 "4th gap": an instance-method spec does not carry the
receiver's element type as a named tyvar / parameterized app the way a plain
constrained-poly defn does. The original pin works around it by re-ascribing
`(:: x (Vec A))` only where it calls the *constrained-poly* helper `callee`
(which needs the named tyvar), and using `(:: x :int)` + the bridge for the
*carrier* helpers.

### Gap 2 -- with `(:: x (Vec A))` the redirect fires but the twin clone collides across ABIs

Re-ascribing the carrier-helper call to the parameterized form,
`(vec-len (:: x (Vec A)))`, gets the redirect past Gap 1:

```
[redirect] vec-len resolved_recv.kind=21 extract=1   // TY_APP, Vec int extracted
```

But the build still fails:

```c
static int64_t vec_len_byval__spec__int64_t_int64_t(int64_t v);   // <- int64_t param!

// carrier base (x is int64 carrier) -- consistent:
return callee__spec__...((*(Vec__int*)(intptr_t)x), ...,
                         vec_len_byval__spec__int64_t_int64_t((*(int64_t*)(intptr_t)x)));
// by-value spec (x is Vec__int) -- cc error:
return callee__spec__...(x, y, INT64_C(0),
                         vec_len_byval__spec__int64_t_int64_t(x));
//   error: incompatible type for argument 1 ... expected 'int64_t' but argument is 'Vec__int'
```

The `(vec-len (:: x (Vec A)))` **source call node** is emitted in *both* the
carrier base (`__inst_MyEq_myeq_qu_Vec`, int64 receiver) and the by-value spec
(`__inst_..._spec__bool_Vec__int_Vec__int`, `Vec__int` receiver). The twin
redirect records a single clone name per call node via
`emit_abi_record_specialized_call(ctx, call, clone_name)`, so both emission
contexts read the *same* twin clone -- here the `int64_t`-param one minted from
the carrier-base scan. The by-value spec then passes `Vec__int` into an
`int64_t` formal.

This is the `(call-node, active-spec)` keying fragility recorded as Finding 7
in `docs/upcoming/m5-residual-straddle-retirement.md` (session 4 cont. 3),
recurring specifically for the **twin-redirect** path interacting with a
dual-ABI (carrier base + by-value spec) instance method. The Finding-7 fix
that landed in `deee4c6` keyed *regular* spec clones per `(call, active-spec)`;
the twin-redirect's `emit_abi_record_specialized_call` was not brought under
the same keying.

## Why the bridge is therefore load-bearing

With both gaps in place, the explicit `(:: x :int)` + EX_ASCRIBE bridge is the
*only* working way to call a carrier helper on a by-value receiver from an
instance-method spec body. The two pins are not redundant with
`m5-option-c-vec-byval-redirect` (which exercises a *plain* constrained-poly
defn spec, where the redirect does fire cleanly). Deleting the bridge today
turns `(:: x :int)`-on-a-by-value-struct into a hard `cc` type error with no
clean Turmeric diagnostic.

## Fix directions

Two coherent ways to finish D.4 for the EX_ASCRIBE branch; pick one:

1. **Extend the redirect, then migrate the pins, then delete the bridge.**
   - Gap 1: make the redirect resolve the element type for an instance-method
     spec receiver that arrives as an element-erased `TY_STRUCT` (e.g. consult
     the active spec's receiver binding / the `__spec__..._Vec__int` suffix, or
     bring the instance receiver's type-ctor params into scope as named tyvars
     the way the MutableMap fix did for multi-param instances).
   - Gap 2: key `emit_abi_record_specialized_call` for the twin redirect on
     `(call-node, active-spec)`, mirroring the Finding-7 fix, so the carrier
     base and the by-value spec each get their own twin clone.
   - Then both pins can drop `(:: x :int)` (Gap-1 fix) and compile through the
     redirect; retire/repurpose them and delete the EX_ASCRIBE
     `CK_CONCRETE -> CK_CARRIER` branch + the symmetric M3 accessor-side path.
   - This is real emit work, roughly the same class as the rest of M5 -- *not*
     a mechanical pin retirement.

2. **Keep the bridge as supported surface; close D.4-for-EX_ASCRIBE as won't-do.**
   Treat explicit `(:: <by-value-carrier-struct> :int)` widening as a
   legitimate, documented construct (the two pins are its regression tests),
   and record that the EX_ASCRIBE `CK_CONCRETE -> CK_CARRIER` site stays. The
   audit's "bridge count -> 0" north star then becomes "bridge count -> N, all
   genuinely type-erasing/widening constructs" -- consistent with how the plan
   already keeps the existential / `tur_poly_fn_t` carriers. If chosen, also
   add an elaborator-level rejection (clean diagnostic) for the *unsupported*
   shapes so a stray `(:: x :int)` can never silently reach a cc error.

## How to validate a fix

- Direction 1: both pins, rewritten to drop `(:: x :int)` (calling
  `(vec-len x)` / `(vec-get x i)` directly), build and run to their
  `expected.stdout`; the EX_ASCRIBE `CK_CONCRETE -> CK_CARRIER` probe fires for
  **zero** fixtures; `bash tests/run.sh` green; snapshots regenerated.
- Direction 2: the pins keep their explicit ascriptions and stay green; the
  decision + rationale is recorded in the audit and the M5 retirement doc, and
  the EX_ASCRIBE site is moved from the "to retire" column to the "kept carrier
  surface" column.

## Repro

```sh
TUR=./build/tur
# Gap 1: redirect bails on element-erased instance-method receiver
sed 's/(vec-len (:: x :int))/(vec-len x)/; s/(vec-get (:: x :int) i)/(vec-get x i)/g' \
  tests/fixtures/m5-instance-spec-constraint-var/input.tur > /tmp/g1.tur
$TUR build /tmp/g1.tur -o /tmp/g1   # cc error: vec_hylen(Vec__int)

# Gap 2: re-ascribe to (Vec A) -> redirect fires but twin clone collides
sed 's/(vec-len (:: x :int))/(vec-len (:: x (Vec A)))/' \
  tests/fixtures/m5-instance-spec-constraint-var/input.tur > /tmp/g2.tur
$TUR build /tmp/g2.tur -o /tmp/g2   # cc error: vec_len_byval__spec__int64_t_int64_t(Vec__int)
```

## Resolution 2026-06-15 (Direction 1)

Took Direction 1. The fix turned out to be **Gap 1 only** -- Gap 2 does not
arise on the chosen migration path.

### Gap 1 fix -- redirect resolves the element-erased receiver via spec arg_types

`emit_abi_try_byval_twin_redirect` (`emit_module.c`) resolved each call arg
type by instantiating the call arg's *elab-time* type through the active spec's
tyvar bindings. For an instance-method spec receiver that elab type is the
element-erased bare `TY_STRUCT Vec` (no tyvar to substitute), so struct-app
extraction failed and the redirect bailed.

Fix: when a call arg is a bare `EX_VAR` bound to a parameter of the active
spec's `fn`, prefer the spec's own concrete `arg_types[pi]` (e.g. `Vec__int`)
over the instantiated elab type. This mirrors what the (now-deleted) M4c
Path A and EX_ASCRIBE bridges already did to recover the monomorphized param
type, and lets the redirect fire for `(vec-get x i)` / `(vec-len x)` inside
`__inst_*__spec__*` exactly as it already did for a plain constrained-poly
defn spec. ~12 lines in the redirect's arg-resolution loop.

### Gap 2 did NOT block -- it was an artifact of the `(:: x (Vec A))` form

The Gap 2 repro re-ascribed the carrier-helper call to `(:: x (Vec A))`, which
made the redirect intern an `int64_t`-param twin and collide across the two
ABIs. The chosen migration does **not** use that form: it drops the ascription
entirely (`(vec-len x)`), so the Gap 1 fix resolves the receiver straight to
`Vec__int` and interns a *single* `vec_len_byval__spec__int64_t_Vec__int`
clone. The carrier base and the by-value spec share that one clone -- the
carrier base deref-bridges its int64 receiver (`*(Vec__int*)x`) at the call
site (the existing `CK_CARRIER -> CK_CONCRETE` arg adaptation), the spec passes
it direct. No collision; the per-call-node clone recording is correct as-is.
So no `emit_abi_record_specialized_call` keying change was needed.

### Pins migrated, bridge deleted

- `m5-instance-spec-constraint-var`: rewritten to the bare idiom
  (`(vec-get x i)`, `(vec-len x)`); now also pins the Gap-1 instance-method-spec
  redirect coverage in addition to gap-4 constraint-var propagation.
- `m5-spec-body-ascription-bridge` -> renamed `m5-spec-body-byval-redirect`,
  rewritten to the bare idiom; pins the redirect for a plain constrained-poly
  defn whose carrier-helper result carries the element type in *return*
  position (`at1-helper [A] [x : (Vec A)] : A`).
- The EX_ASCRIBE `CK_CONCRETE -> CK_CARRIER` producer bridge in `emit_expr.c`
  is **deleted** (replaced with an explanatory comment). An emit-c sweep over
  the full suite (incl. `-Xdata-literals`) confirms zero producers.

The symmetric `CK_CARRIER -> CK_CONCRETE` accessor-side path stays -- it is
still load-bearing (e.g. the `vec-len-byval` carrier base derefs `(:: x (Vec
A))` through it) and is M3's separate target, not part of this D.4.

### Residual (minor)

Deleting the bridge means a *hand-written* `(:: x :int)` on a by-value struct
(only meaningful inside a by-value spec body, which no idiomatic code writes)
now falls through to a carrier-relabel and would surface as a `cc` type error
rather than a clean Turmeric diagnostic. No in-tree code hits this. If it ever
matters, add an elaborator-level rejection per Direction 2's diagnostic note.

### Validation

`bash tests/run.sh`: 1636 passed, 0 failed, zero codegen-snapshot drift. Both
migrated pins build and run to their `expected.stdout` on the compiled backend.
