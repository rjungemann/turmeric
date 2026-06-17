---
title: By-value `(Option A)` param could not receive a carrier `some`/`none` (cc error; none-as-NULL deref hazard)
category: Codegen / ABI -- Option none-as-NULL retirement (Track A)
severity: Medium. A plain (non-spec) function declared with a concrete by-value
  `(Option A)` (or `(Result A B)`, `(Pair A B)`, ...) parameter could not be
  called with a carrier construct result (`(some x)` / `(none)` / `(ok x)`):
  the call site emitted the int64 carrier straight into the by-value formal, a
  hard cc type error (`incompatible type ... expected 'Option__int' but argument
  is of type 'int64_t'`). This is the root reason the stdlib Option consumers
  (`some?`, `unwrap-or`, `option-map`, `option-eq?`) are still typed `o : int`
  (the historical carrier ABI / a No-Lazy-`:int` stand-in): a by-value `(Option
  A)` signature was simply not callable from carrier producers. Worse, the same
  boundary is where the documented "none-as-NULL" deref hazard lived -- a carrier
  `none` (int64 `0`) reaching a by-value Option sink risked `*(Option__A *)
  (intptr_t)(0)`.
status: RESOLVED 2026-06-17 (this session). The call-arg emitter now bridges a
  carrier construct result into a concrete by-value aggregate param for PLAIN
  (non-spec) callees, reusing the existing NULL-safe carrier->concrete bridge
  (carrier `0` == none reconstructs to `(Option__A){0}`, never deref'd). See
  "Resolution" below.
---

# By-value `(Option A)` params: carrier construct results now bridge safely

## Symptom

```turmeric
(defn takes-byval [o : (Option int)] : int
  (if (.is-some o) (.value o) -1))

(defn main [] : int
  (println (takes-byval (some 5)))                  ; cc error before this fix
  (println (takes-byval (:: (none) (Option int))))  ; cc error / NULL-deref hazard
  0)
```

Emitted (before):

```c
static int64_t takes_hybyval(Option__int o) { ... }
...
printf("%lld\n", (long long)(takes_hybyval(some(INT64_C(5)))));  // int64_t -> Option__int : ERROR
printf("%lld\n", (long long)(takes_hybyval(none())));            // int64_t (0) -> Option__int : ERROR
```

`some` / `none` are `#{Construct}` constructors whose CARRIER base returns the
int64 carrier (`some(x)` -> `tur_some(x)` heap box; `none()` -> `0`). When the
consuming param is a concrete by-value `(Option int)` (-> `Option__int`), the
carrier int64 was passed raw into the struct formal.

## Root cause

The call-arg emitter's carrier->concrete bridge (`src/compiler/emit_expr.c`,
the `matched_spec` branch around the `emit_carrier_bridge(CK_CARRIER,
CK_CONCRETE, matched_spec->arg_types[i])` call) fired **only when the callee was
an ABI specialization** (`matched_spec` set). A plain monomorphic user function
with a concrete by-value aggregate param has no `matched_spec`, so the bridge
never fired and the carrier arg hit the struct formal unconverted.

This is *the* reason the stdlib Option consumers are typed `o : int`: a by-value
`(Option A)` signature was uncallable from `some`/`none`, so the carrier `:int`
was the only working consumer ABI -- a No-Lazy-`:int` stand-in forced by a
codegen gap, not a design choice.

## Resolution

Added a non-spec branch to the carrier->concrete arg bridge in
`src/compiler/emit_expr.c`: when the callee has no `matched_spec` but its
declared param type (`fn_binding->type.as.fn.arg_full_types[i]`) is a concrete
by-value aggregate (`type_kind_is_aggregate` + `type_has_concrete_codegen_layout`
+ not `:heap`), and the emitted arg is a carrier (TY_INT, or a carrier-ABI
aggregate that is not a by-value producer), bridge it `CK_CARRIER ->
CK_CONCRETE` using the declared param type. The condition on `emit_arg` mirrors
the proven spec branch exactly (`!expr_emits_byvalue_carrier_abi` excludes a
genuine by-value arg, so existing by-value pass-through is untouched).

The existing bridge already reconstructs Option/Result field-by-field from the
canonical carrier box (`tur_option_t` / `tur_result_box_t`) and **NULL-guards
Option**: `(__t ? (Option__A){.is_some = __t->is_some, .value = ...} :
(Option__A){0})`. So a carrier `none` (`0`) reconstructs to the zeroed by-value
Option (`is_some = false`) without ever being deref'd -- the none-as-NULL hazard
is gone at this boundary. Float payloads reconstruct via the union reinterpret
(verified: `7.25` round-trips, `none` -> `{0}`).

Emitted (after):

```c
tur_option_t *__t30 = (tur_option_t *)(intptr_t)(some(INT64_C(5)));
takes_hybyval((__t30 ? (Option__int){.is_some = __t30->is_some, .value = __t30->value} : (Option__int){0}));
tur_option_t *__t31 = (tur_option_t *)(intptr_t)(none());
takes_hybyval((__t31 ? (Option__int){.is_some = __t31->is_some, .value = __t31->value} : (Option__int){0}));
```

## Validation

- New fixture `tests/fixtures/option-byvalue-param-none-safe` (int + float
  payload, `some` + `none`): prints `5 / -1 / 725 / -100`. Snapshot pins the
  NULL-safe bridge shape.
- Full compiled suite: green (see commit).
- No existing snapshot drift: the only programs affected are ones that
  previously failed to compile (carrier arg into a by-value aggregate formal),
  so no passing fixture's codegen changes.

## Why this is scoped to the bridge, not the construct-spec

A cleaner end-state would mint a by-value construct spec at the call site
(`(some 5)` -> `(Option__int){.is_some=true,.value=5}` directly, no carrier box,
no bridge). That requires threading the consuming param's expected type into the
ABI scan for construct-call args (`result_type_override`), which fires for every
`(some x)`/`(ok x)` in a by-value argument context project-wide -- the broad M2
suite-wide snapshot regen the monomorphization plan deliberately deferred
(`emit_module.c` `construct_recovered_byvalue` is GATED to instance-method specs
"avoiding the broad M2 suite-wide snapshot blast"). The bridge fix delivers the
correctness + none-safety with zero snapshot churn; the construct-spec
optimization is a separate coordinated-regen decision.

## Remaining (the Option-consumer retype this unblocks)

With by-value `(Option A)` params now callable from carrier producers, the
stdlib Option consumers can be retyped from `o : int` to `o : (Option A)` with
pure-Turmeric by-value bodies (`some?` -> `(.is-some o)`, `unwrap-or` -> `(if
(.is-some o) (.value o) dflt)`, etc.), eliminating the 8 `concrete->carrier`
spills in `option-consumers-byvalue-arg`. That retype has a wide blast radius
(every carrier caller of these consumers) and is the next Track A increment; it
is gated only on this codegen fix, which is now in tree. See
[docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
"Update 2026-06-17 (post-#411 baseline refresh)".

## Related

- `src/compiler/emit_expr.c` -- the call-arg carrier->concrete bridge (now with
  the non-spec by-value-aggregate branch).
- `src/compiler/emit_core.c` `emit_carrier_bridge` -- the NULL-safe
  Option/Result field-wise reconstruction this reuses.
- [docs/upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)
  §M2 -- the construct-spec direction this complements.
