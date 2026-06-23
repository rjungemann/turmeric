---
title: Typeclass instance method over a by-value parametric struct takes the int64 carrier, but a by-value struct-field read passes the aggregate -- ABI mismatch
severity: medium -- blocks composing a container instance (Enc [(Option cstr)]) over a struct FIELD of that type. Dispatch over a *local* of the same type works; only the struct-field receiver path mismatches. Continuation of the carrier-vs-byvalue family (#475/#479/#480/#481), on the instance-method-receiver ABI side.
status: RESOLVED 2026-06-21 (gap G3). Fixed on
  branch claude/g2-carrier-concrete-abi-audit-3yzkhm; fixture
  tests/fixtures/instance-method-byvalue-struct-field-receiver. See
  "Resolution" below.
discovered: 2026-06-21
surfaced-by: composing Enc/Encode [(Option cstr)] over a defstruct field, after (a) the by-value parametric-struct field layout fix (struct-field-byvalue-parametric-struct-layout) made `(.field x)` yield the real Option__cstr aggregate and (b) the applied-unary-instance-head kind fix (applied-unary-instance-head-promotes-star-class) let the instance set elaborate. Both predecessors are resolved; this is the next layer they expose.
---

# Instance method receiver ABI mismatch over a by-value struct-field read

## One-line summary

A typeclass instance whose head is a by-value parametric struct
(`Enc [(Option cstr)]`) emits its method with the int64 **carrier** ABI
(`enc(int64_t x)`).  Dispatch over a *local* of that type passes the carrier
and works.  But a struct **field** of that type now reads back as the real
by-value aggregate (`Option__cstr`, post struct-field-byvalue-parametric-struct-layout),
so the call passes an aggregate into the int64 parameter and the C compiler
rejects it.

## Minimal repro

```turmeric
(defclass Enc [a] (enc [x] : cstr))
(definstance Enc [(Option cstr)] (enc [x] : cstr (if (.is-some x) "s" "n")))
(defstruct Rec [nick : (Option cstr)])
(defn main [] : int
  (let [r (make-struct Rec (:: (some "al") (Option cstr)))]
    (println (enc (.nick r))))         ; <- ABI mismatch here
  0)
```

Emitted-C error:

```
error: incompatible type for argument 1 of '__inst_Enc_enc_Option_cstr'
note: expected 'int64_t' but argument is of type 'Option__cstr'
```

The instance method is declared `static const char *
__inst_Enc_enc_Option_cstr(int64_t x);` while the call site is
`__inst_Enc_enc_Option_cstr((r).nick)` with `(r).nick` of type
`Option__cstr`.

## Control (works)

Dispatch over a *local* of the same type compiles + runs:

```turmeric
(let [o (:: (some "hi") (Option cstr))]
  (println (enc o)))                   ; prints "s"
```

Here the receiver flows through the carrier ABI consistently, so the
int64 parameter matches.  (A residual `-Wint-conversion` warning remains in
the instance body where `(.value x)` -- a by-value Option field read as the
int64 carrier -- is passed to `enc [cstr]`; same duality, non-fatal.)

## Diagnosis / fix direction

The instance method parameter for a by-value (non-`:heap`) parametric struct
receiver should agree with how that value is materialized at the call site.
Two consistent options:

1. Emit the method with the concrete by-value parameter (`Option__cstr x`)
   and bridge carrier-ABI call sites (locals) into the aggregate; or
2. Keep the carrier parameter and bridge the by-value struct-field read back
   to the carrier at the dispatch site (`(int64_t)(intptr_t)` won't do for a
   multi-word aggregate -- it would need a spill/box).

Option 1 matches the direction the struct-field and make-struct paths already
took (embed the aggregate, monomorphized per type).  See the sibling
resolved reports in docs/archive/history/ for the make-struct / field-layout
precedent.

## Scope

Compiler-side (typeclass instance method ABI vs. by-value aggregate
materialization).  Unblocks composing `Encode`/`Decode [Option]` over
`derive-json` struct fields end-to-end in turmeric-spices spices/json (the
field LAYOUT already works; this is the dispatch-ABI half).

## Resolution (2026-06-21)

Fixed via **option 2** (keep the carrier parameter; bridge the by-value
struct-field read to the carrier at the dispatch site -- the multi-word
aggregate is spilled to a temp and its address passed, exactly as a by-value
LOCAL of the same type already was).

The one-line change is in `expr_emits_byvalue_carrier_abi` (`emit_expr.c`): a
post-#482 by-value (non-`:heap`) aggregate **field read** (`(.nick r)`, whose
resolved type is a concrete non-heap struct with a real codegen layout) is now
recognized as a by-value carrier producer. The existing concrete->carrier bridge
(`emit_carrier_bridge`) then spills it and passes `(int64_t)(intptr_t)(&tmp)`,
matching the instance method's `int64_t` parameter (which the body reads back as
the aggregate pointer). A `:heap` field (Cons/Vec) is excluded -- it is still
carried as the int64 pointer. The residual `-Wint-conversion` in the instance
body the report noted is also gone.

The call now emits:

```c
puts(__inst_Enc_enc_Option_cstr((int64_t)(intptr_t)(&__t43)));   // __t43 = (r).nick
```

Fixture `tests/fixtures/instance-method-byvalue-struct-field-receiver` (local
control + some/none cstr field). Suite green (1741/0).

**Distinct, still-open neighbor:** two applied-struct instances of one class
differing only in the element (`Enc [(Option cstr)]` + `Enc [(Option int)]`)
conflate at dispatch (the selection key drops the element). Verified
pre-existing; tracked as gap G10
(`docs/reported/multiple-applied-struct-instances-same-class-conflate.md`). The
G3 fixture deliberately uses a single applied-struct instance to avoid it.
