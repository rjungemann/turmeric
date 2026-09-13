# A generic wrapper tail-forwarding a return-dispatch method leaks a boxed struct payload

**Severity: low** -- 16 bytes per call, and only when the `Result`/`Option`
payload is a by-value struct. Scalar and `cstr` payloads are leak-clean.

**Status:** open, and **narrowed** from the original report. The hard cc error
this file was opened for is **fixed** (see below); what survives is a missing
caller-side free on one payload shape.

## History: the cc error (FIXED 2026-09-13)

Originally this shape did not compile at all:

```turmeric
(defclass DecodeJson [a] (decode-json [doc : int  val : int] : (Result a cstr)))
(definstance DecodeJson [int] (decode-json [doc val]
  ```c
  (void)doc; return tur_box_ok((int64_t)(val + 100));
  ```))

;; A plain generic wrapper over the method -- the encode-string / decode-list
;; shape from json/encode.tur.
(defn decode [A] [(DecodeJson A)] [doc : int  val : int] : (Result A cstr)
  (decode-json doc val))
```

```
error: incompatible types when returning type 'int64_t' {aka 'long int'}
       but 'tur_adt_Result__int__cstr' was expected
```

The instance returns the int64 carrier (a heap Result box); the wrapper's
monomorphized spec declares the by-value aggregate, and the return position
had no bridge -- a concrete `(:: (decode-json d v) (Result int cstr))` gets the
box readback from the ascription bridge, but the spec's `return` did not.

Fixed by a third clause in `fn_return_needs_carrier_result_bridge`
(`src/compiler/emit_fns.c`), alongside the existing catch-box and
raw-slot-read clauses: when the tail value is an `int64_t` temp and the
declared result is `REPR_BYVAL_AGG`, route the return through
`emit_carrier_bridge` (CK_CARRIER -> CK_CONCRETE), which emits the same
NULL-guarded deref-and-free the ascription site already gets. Reaching that
point with an int64 temp and an aggregate return type was always the
miscompile -- `return <int64_t>` into a by-value struct has no valid reading
-- so the clause can only turn a guaranteed cc failure into the readback the
value needs.

Pinned by `tests/fixtures/generic-wrapper-tail-forwards-return-dispatch`.
Full suite green (2971 passed, 0 failed).

## What remains: the boxed-struct payload leaks

With the bridge in place the program runs and answers correctly, but a
`Result` whose Ok payload is a **by-value struct** leaks that payload's box:

```
Direct leak of 16 byte(s) in 1 object(s):
    #1 tur_region_alloc_or_malloc
    #2 ok__spec__int64_t_tur_adt_User
    #3 __inst_DecodeJson_decode_hyjson_User
    #4 decode__spec__tur_adt_Result__User__cstr_int64_t_int64_t
```

Verified with ASan/LSan: scalar and `cstr` payloads are clean; only the struct
payload leaks. The destructure-and-rebuild spelling is clean for every payload.

## Root cause of the remaining leak

Two allocations are in play, not one:

- the **carrier box** holding the Result -- the new return bridge frees this,
  correctly and exactly once; and
- the **payload box**, because a monomorphized `Result` over a by-value struct
  stores its Ok arm as a `tur_adt_User *` and the ctor mallocs a fresh copy
  into it (`ok__spec__int64_t_tur_adt_User`).

The payload box is meant to be released by the **caller**, at the let-binding
that owns the returned aggregate: `emit_let_value` (`emit_expr.c:3149`) emits
`boxed_struct_payload_walk`'s tag-switch free when
`adt_app_has_boxed_struct_payload(b->type)` **and**
`emit_init_owns_fresh_sum(ctx, init)` both hold.

The second is what fails. It reduces to `fb->returns_fresh_sum_box` on the
callee's binding, set at elaboration from the wrapper's body. For the
destructure-and-rebuild body the tail is an `ok` / `err` ctor call, so the flag
is set and the caller emits the free. For the direct tail-forward the tail is a
class-method call whose result is the abstract class tyvar, so elaboration
cannot see a fresh producer and the flag stays clear -- no caller-side drop.

## Fix directions

The wrapper's spec *always* hands back a freshly-owned aggregate after the
bridge (the carrier box is freed and the payload pointer inside has no other
owner), so in principle the binding should be flagged `returns_fresh_sum_box`.

**Not attempted here, deliberately.** The flag is consulted per call site and
drives a `free`; setting it where an instance hands back a *borrowed* box
(the `vec-get` shape, which the carrier bridge's own ownership mark exists to
distinguish) would turn a 16-byte leak into a double free. The bridge knows
which case it is at emit time; the flag is set at elaboration and read at other
call sites, so the two need to be connected deliberately rather than by
pattern-matching the tail. That is the work, and it wants someone with the
ownership subsystem in view -- not a quick follow-on to the codegen fix.

Until then, `tests/fixtures/generic-wrapper-tail-forwards-return-dispatch`
carries `requires.no-leak-check`, and the destructure-and-rebuild spelling
(pinned by `tests/fixtures/generic-wrapper-over-return-dispatch-method`)
remains the leak-clean choice when a wrapper returns a struct payload.
