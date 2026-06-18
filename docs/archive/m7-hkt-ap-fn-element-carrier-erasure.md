---
title: M7 HKT Applicative `ap` blocked -- function element of an HKT container erases to `ptr<void>`
severity: expressiveness gap (blocked the Applicative `ap` shape under the M7 by-value HKT path) -- RESOLVED 2026-06-18
status: RESOLVED (2026-06-18) -- fix directions 1 + 3 landed; `ap` monomorphizes by value (probe -> 42)
since: 2026-06-18
---

## Resolution (2026-06-18) -- RESOLVED, both halves landed

The `ap` probe (`docs/upcoming/v2/m7-hkt-probe-ap.tur`) now exits **42** under
`TUR_M7_HKT=1` with no ascription. Flag-off path byte-identical (suite 1683/0);
flag-on codegen across a 92-fixture HKT/typeclass sweep is unchanged vs. the
parent commit (the fix only enables the previously-broken `ap` shape).

**Fix direction 3 (defensive guard) -- landed first.** A residual free result
element tyvar aborts the by-value HKT monomorphization and falls back to the
uniform carrier dispatch, instead of emitting a half-by-value spec with a
dangling carrier-base dict reference (`m7_type_has_free_tyvar` +
`m7_byvalue_grounded` in `src/compiler/elab_typeclasses.c`,
`elab_method_call`). This converted the original hard cc error into a clean
diagnostic and -- crucially -- still backstops the genuinely-uninferable case
(see Caveat below).

**Fix direction 1 (preserve the fn type) -- landed, three coordinated pieces:**

1. **Producer (`src/compiler/elab_call.c`).** A bare `TY_FN` escaping into a
   `TY_TYVAR` parameter is boxed via `EX_FN_TO_FAT`; the shim's STATIC type now
   keeps the precise `(fn [int] int)` signature (marked `boxed`) instead of
   erasing to `ptr<void>`. So `some`'s `A` binds to the real fn type and
   `(some add1) : (Option (fn [int] int))`. The runtime value is still a fat
   box (`EX_FN_TO_FAT` emits a `void *` regardless, reading `inner->type`);
   only the static type changes, gated behind `g_m7_hkt_enabled`.
2. **Call site.** With the producer type preserved, `m7_collect_tyvar_bindings`
   unifies decl `(f (fn a b))` against actual `(Option (fn int int))` and now
   recovers `b` (the wrapped fn's RETURN type), so the result `(f b)` grounds to
   `(Option int)` and the by-value spec returns `Option__int`.
3. **Instance body (`src/compiler/elab_typeclasses.c`).** The body param
   `ff : (Option (fn a b))` has its element fn marked `boxed`
   (`m7_box_hkt_element_fns`, applied to `elab_param_type` under the flag) so
   calling the wrapped function -- `((.value ff) (.value fa))` -- dispatches
   through the fat-box thunk instead of bare-calling the box address (which read
   the box pointer as code and segfaulted).

**Verified:** `b = int` (probe -> 42), `b = cstr` (the wrapped fn's RETURN type
drives `b` independently of `a = int`), and the `(some f)/(none)` short-circuit.

**Caveat (legitimate inference boundary, not a regression).**
`(ap (none) (some 41))` supplies no function anywhere, so `b` is genuinely
uninferable; the fix-direction-3 guard keeps the result un-grounded and emits a
clean `(type-app ? ?)` type error rather than a miscompile. The user annotates
the `(none)` (or passes a real function) in that case. This is the correct
behavior for an under-determined type, identical in spirit to needing an
annotation on a bare `(none)` anywhere else.

---

## (original report follows)

# M7 HKT `ap`: the wrapped-function element of `(f (fn [a] b))` erases to `ptr<void>`

## One-line summary

Under the M7 by-value HKT path (`TUR_M7_HKT=1`), the Applicative `ap` shape
`ap [ff : (f (fn [a] b)) fa : (f a)] : (f b)` cannot monomorphize by value:
the actual value `(some add1)` elaborates to `(Option ptr<void>)`, erasing the
wrapped function's signature, so the result element tyvar `b` is unrecoverable
at the dispatch call site.

## Severity

Expressiveness gap. It blocks the `ap` (Applicative) member of the M7
probe-driven layer-4 hardening (Phase 4.2 of
`docs/upcoming/end-to-end-monomorphization-plan.md`). The `fmap` (Functor) and
`bind` (Monad) shapes already work end-to-end under the flag; `ap` is the next
shape and is the first to put a *function value* in the wrapped element
position.

With the flag OFF the shipped path is byte-identical (the probe is not a suite
fixture). With the flag ON it is currently a **hard cc error**, not a silent
miscompile (see "Observed" below), so it does not violate the
no-silent-miscompile rule -- but it does mean `ap` cannot be admitted to the
by-value gate yet.

## Minimal repro

`docs/upcoming/v2/m7-hkt-probe-ap.tur` (kept in-repo, deliberately not a suite
fixture):

```turmeric
(defclass MyApplicative [^f]
  (ap [ff : (f (fn [a] b)) fa : (f a)] : (f b)))

(definstance MyApplicative [Option]
  (ap [ff fa]
    (if (some? ff)
      (if (some? fa)
        (some ((.value ff) (.value fa)))
        (none))
      (none))))

(defn add1 [x : int] : int (+ x 1))

(defn main [] : int
  (let [r (ap (some add1) (some 41))]
    (unwrap r)))   ; TARGET: 42
```

Run: `TUR_M7_HKT=1 ./build/tur run docs/upcoming/v2/m7-hkt-probe-ap.tur`

## Observed vs. expected

**Expected:** exit 42 (mirroring `m7-hkt-probe.tur` -> 42 and
`m7-hkt-probe-bind.tur` -> 21).

**Observed (flag ON):** cc invocation fails. Two symptoms in the generated C:

1. The per-instance dispatch dict references a carrier base method that was
   never emitted:
   ```
   error: '__inst_MyApplicative_ap_Option' undeclared here (not in a function)
       .ap = __inst_MyApplicative_ap_Option,
   ```
2. The interned by-value spec returns the int64 carrier and treats the wrapped
   function as a raw int64:
   ```
   static int64_t __inst_MyApplicative_ap_Option__spec__int64_t_Option__opaque_Option__int(
       Option__opaque ff, Option__int fa) { ... }
   ...
   int64_t (*__call_head)(int64_t) = (ff).value;   // int64 -> fn ptr, no cast
   ```
   Note the spec name: arg1 is `Option__opaque` and the **return is
   `int64_t`** (the carrier), not the by-value `Option__int` that the `fmap`
   probe produces.

## Root-cause analysis

Instrumented `m7_collect_tyvar_bindings` at the dispatch call site
(`src/compiler/elab_typeclasses.c`, ~line 4827, `elab_method_call`). The
collector unifies the CLASS method's declared param types against the actual
call-arg types to bind the element tyvars (`f`, `a`, `b`), then substitutes
them into the declared result `(f b)`.

Traced types:

```
decl param[0]  = (f (fn a b))     ; element TY_FN: arg0_full=TY_TYVAR 'a',
                                  ;                result_full_type=TY_TYVAR 'b'
decl param[1]  = (f a)            ; TY_APP(f, TY_TYVAR 'a')
obj_orig (ff)  = (Option ptr<void>)   ; <-- the wrapped fn is erased
arg_orig (fa)  = (Option int)
```

- **The decl side is correct.** `parse_typeclass_method` with the method-tyvar
  threading (`m7_collect_form_tyvars`) preserves `a` and `b` as named TY_TYVARs
  inside the nested fn `(fn a b)`. (A `type_print` dump shows `(fn [int] : int)`
  only because `type_print`'s TY_FN branch is kind-only / lossy --
  `src/compiler/types.c:2723` -- the `arg_full_types`/`result_full_type` carry
  the real tyvars, verified by direct field inspection.) **No decl-side bug.**

- **The value side erases.** `(some add1)` elaborates with type
  `(Option ptr<void>)`. `some` is `[A] [x : A] : (Option A)`; passing a function
  value to the type-variable parameter `A` records the arg as the universal poly
  carrier `ptr<void>` (cf. the "type variable -> universal int64_t" / "rank-2 fn
  -> tur_poly_fn_t" lowering in `src/compiler/elab_types.c`), dropping the
  precise `(fn [int] int)` signature.

- **Therefore `b` is unrecoverable.** `b` (the result element = the wrapped
  fn's RETURN type) lives only inside that erased fn. Unifying decl
  `(f (fn a b))` against actual `(Option ptr<void>)`: `f -> Option` binds, but
  the nested `(fn a b)` vs `ptr<void>` is fn-vs-non-fn, so `a` and `b` stay
  free. `fa : (f a)` vs `(Option int)` recovers `a -> int` but nothing recovers
  `b`.

- **Downstream consequences.** With `b` free, the substituted result `(f b)`
  resolves to `(Option b)` whose `type_c_name` is the int64 carrier, so:
  (1) the by-value spec emits an `int64_t` (carrier) return rather than
  `Option__int` by value; and
  (2) `emit_abi_note_carrier_call` (`src/compiler/emit_module.c:1752`) -- which
  keeps the carrier base method alive for the dispatch dict -- is gated on
  `result_type.kind == TY_APP && type_has_concrete_codegen_layout(...)`, which
  the unresolved `(Option b)` fails, so the base method is dropped while the
  dict still references it (symptom 1).

This is distinct from the `fmap`/`bind` shapes, where the result element `b`
is recoverable from a *non-function* wrapped value (`fmap`: `(f a)` arg;
`bind`: the continuation's explicit `: (Option b)` return annotation). `ap` is
the first shape whose `b` is reachable only *through* an erased function value.

## Proposed fix directions

1. **Thread the precise function type through polymorphic constructor calls.**
   Make `(some add1)` elaborate to `(Option (fn [int] int))` (preserve the
   wrapped fn's full signature when binding a type variable to a function
   value), so `m7_collect_tyvar_bindings` recovers `b` structurally. This is the
   "fat-closure carrier" follow-on already flagged in Phase 4.2 of the
   monomorphization plan; it is a broad representational change (every poly call
   that binds a tyvar to a function value), not a layer-4 emit tweak.

2. **Recover `b` from the instance body's construct site instead of the args.**
   The body's `(some ((.value ff) (.value fa)))` constructs the result element
   from `(.value ff)` applied -- if the wrapped fn type were available there,
   `b` could be read off the application's result type. This still needs the fn
   element type preserved somewhere, so it reduces to (1).

3. **Defensive guard (not a fix):** when an HKT by-value spec's result element
   stays a free tyvar, *abort the by-value monomorphization for that call and
   fall back to the carrier dispatch* rather than emitting a half-by-value spec
   with a dangling carrier-base dict reference. This converts the current cc
   error into a clean carrier fallback (no by-value `ap`, but no broken C
   either) and would let the flag tolerate `ap` instances until (1) lands.

## How to validate a fix

- `TUR_M7_HKT=1 ./build/tur run docs/upcoming/v2/m7-hkt-probe-ap.tur` exits 42
  with no ascription, and the generated spec returns `Option__int` by value
  (mirroring the `fmap` spec naming `..._Option__int`, not `..._int64_t`).
- Extend coverage to `b != a` and `b = cstr` (e.g. `(some int->str)`) to prove
  the wrapped fn's return type -- not just its arg type -- drives `b`.
- Flag-OFF suite stays `1683 passed, 0 failed` (byte-identical shipped path).
- `bash tests/run.sh` clean (10-minute timeout).

## Pointers

- Probe: `docs/upcoming/v2/m7-hkt-probe-ap.tur`
- Call-site element-tyvar collection: `src/compiler/elab_typeclasses.c`
  `m7_collect_tyvar_bindings` (~1687) and its use in `elab_method_call` (~4827).
- Carrier-base-preservation gate: `src/compiler/emit_module.c:1752`
  (`emit_abi_note_carrier_call`).
- Lossy TY_FN print: `src/compiler/types.c:2723`.
- Plan context: `docs/upcoming/end-to-end-monomorphization-plan.md` Phase 4.2
  ("APPROACH UPDATE (2026-06-19)").
