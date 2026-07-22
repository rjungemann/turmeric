# Inline-C `(Option String)` (carrier) can't be passed to a by-value `(Option String)` parameter

> **Status: RESOLVED 2026-07-22.** The compiler now bridges the carrier into the
> by-value aggregate at the call-argument boundary, in both the by-value-param
> and the wide pass-by-pointer-param cases. The workaround (assembling the Option
> in Turmeric over a low-level `String` helper) has been retired -- the httpd
> Part 2 accessors return `(Option String)` directly from inline-C via
> `tur_some_ptr` / `tur_none`. Full suite 2257 passed, 0 failed. See the
> Resolution section at the bottom. This file is retained for the paper trail;
> per the archiving rule it lives in `docs/archive/`.

**Severity:** Medium -- a real expressiveness hole for inline-C-built
`(Option T)` / `(Result T E)` where `T` is a by-value HKT-monomorphized type
(e.g. the owned `String`). Had a clean workaround (assemble the Option in
Turmeric over a low-level pointer/handle helper), so it did not block v1.

## Summary

An inline-C body that returns `(Option String)` via the documented typed
builders (`tur_some_ptr` / `tur_none`, per
[docs/guides/inline-c-results-guide.md](../guides/inline-c-results-guide.md))
produces the **carrier** option representation (an `int64_t`, the
`tur_option_t` 2-slot heap box). That flows fine into the stdlib accessors
(`some?` / `unwrap`) at a *direct* call site, but the moment the value is
passed as an argument to a **Turmeric** function whose parameter is typed
`(Option String)`, codegen expects the **by-value** monomorphized struct
`tur_adt_Option__String` and the C compiler rejects the `int64_t`:

```
error: incompatible type for argument 1 of 'use_hyhelper'
note: expected 'tur_adt_Option__String' but argument is of type 'int64_t'
```

`(Option Device)` where `Device` is a plain `(defopaque :ptr<void>)` does
**not** trip this (see `tests/fixtures/inline-c-result-builder`), because that
fixture only consumes the result through the stdlib accessors -- it never
passes the inline-C-built option to a user function typed `(Option Device)`.
The straddle is specifically the **carrier `int64_t` -> by-value ADT struct**
boundary at a Turmeric function argument (and, by symmetry, a `let`-binder or
return of that param type).

## Minimal repro

```turmeric
(load "stdlib/string.tur")

(defn mk [n : int] : (Option String)
  ```c
  if (n == 0) return tur_none();
  return tur_some_ptr(tur_string_from_bytes("hi", 2));
  ```)

(defn use-helper [o : (Option String)] : nil          ; by-value param
  (if (some? o) (println (string/to-cstr (unwrap o))) (println "none")))

(defn main [] : int
  (do
    ;; direct consumption -- OK
    (let [o (mk 1)] (if (some? o) (println (string/to-cstr (unwrap o))) (println "none")))
    ;; passed to a user fn -- FAILS to build (carrier int64 vs tur_adt_Option__String)
    (use-helper (mk 1))
    0))
(main)
```

`tur run repro.tur` (inline-C forces the compiled path) fails in `cc` with the
`incompatible type for argument` error above. Deleting the two `use-helper`
lines makes it build and run.

## Root cause (suspected)

`Option` is `(defstruct Option [A] (is-some :bool) (value A))` -- a by-value
HKT struct. For a concrete `A` like `String`, a Turmeric parameter/return typed
`(Option String)` lowers to the monomorphized `tur_adt_Option__String` struct.
But `tur_box_some` / `tur_none` (what `tur_some_ptr` / `tur_none` wrap) build
the **carrier** `tur_option_t` representation and hand it back as the `int64_t`
carrier. The stdlib accessors accept the carrier (they read the canonical
heap layout), which is why direct consumption works; but there is no
carrier->by-value bridge inserted when the inline-C return crosses into a
by-value `(Option String)` argument slot. This is a sibling of the
`void*`<->`int64` straddles tracked in
[compiled-string-return-int-conversion.md](compiled-string-return-int-conversion.md)
and
[macos-clang-int-conversion-hard-error.md](macos-clang-int-conversion-hard-error.md),
but at the Option/ADT-struct boundary rather than a scalar pointer slot, and it
is a **hard type error** (struct vs int), not a downgradeable `-Wint-conversion`.

## Workaround (used by the httpd Part 2 accessors)

Do not return `(Option T)` from inline-C when `T` is a by-value HKT payload and
the result must be passed around. Instead:

1. an inline-C helper returns the low-level payload -- e.g.
   `httpd-req-cookie-str- : String`, returning the fresh owned `String` on a hit
   and a NULL `String` (`return (int64_t)0;`) on a miss;
2. a Turmeric wrapper assembles the Option natively, so it is the by-value
   representation from the start:

   ```turmeric
   (defn httpd-req-cookie-opt [conn : ptr<void> name : cstr] : (Option String)
     (let [s (httpd-req-cookie-str- conn name)]
       (if (= (:: s int) 0) (none) (some s))))
   ```

`(some s)` / `(none)` built in Turmeric produce `tur_adt_Option__String`
directly, so the value passes through user-function parameters with no
straddle. This is the shape `stdlib/httpd-string.tur`
(`httpd-req-cookie-opt` / `httpd-req-form-opt`) and `stdlib/range-bound-string.tur`
use.

## Fix directions

Insert a carrier->by-value bridge (and the reverse) for `(Option T)` /
`(Result T E)` at the same emit positions the scalar straddle bridges already
cover in `src/compiler/emit_expr.c` / `emit_fns.c`: when an argument/binder/
return expects a by-value ADT-monomorphized option/result struct but the value
is the `int64_t` carrier (an inline-C return built with `tur_box_*`), materialize
the struct from the carrier (read `is_some`/`value` off the canonical layout)
rather than passing the raw `int64_t`. Then the inline-C-results-guide pattern
would work uniformly for by-value payloads, and the Turmeric-wrapper workaround
above could be retired.

## Resolution (2026-07-22)

Fixed exactly along the fix directions above, in the **direct-call argument**
emitter (`src/compiler/emit_expr.c`). The `let` binder already carried the
carrier->by-value bridge (`init_carrier_to_byval`, via
`fn_body_tail_byvalue_carrier_type` + `!fn_body_tail_emits_byvalue_carrier_abi`);
the direct-call arg loop had the *reverse* (`CK_CONCRETE -> CK_CARRIER`) spill
but not the forward one. Two additions, both reusing the exact predicates the
binder uses so they fire on precisely the carrier-producer-of-a-by-value-
aggregate case and nothing else:

1. **By-value param (`<= 16` byte aggregate, e.g. `(Option String)` -> a
   `tur_adt_Option__String` param):** a new `else if` in the carrier-bridge
   chain. When the arg is a carrier producer whose by-value type is known, the
   arg does not itself emit the aggregate, and the callee's *recorded* param
   C-type is a by-value ADT struct (`tur_adt_...`, no `*`, not `int64_t`),
   `emit_carrier_bridge(CK_CARRIER, CK_CONCRETE)` derefs the carrier into the
   aggregate.

2. **Pass-by-pointer param (`> 16` byte aggregate, e.g. `(Result String int)` ->
   `const tur_adt_Result__String__int *`):** the existing pbp spill-to-temp now
   applies the same carrier->concrete bridge to the value before spilling it into
   the by-value temp, so `T __tmp = <carrier>` is no longer an invalid
   initializer.

Both gates are guarded by `!matched_spec` and the by-value-carrier-type
predicate, so ordinary carrier-`:int` consumers, already-by-value args, and
spec-dispatch calls are untouched. Confirmed: the `inline-c-result-builder`
codegen snapshot is unchanged, and the full suite is green.

**Coverage landed with the fix:**

- Regression fixture `tests/fixtures/inline-c-option-byval-param` exercises both
  the by-value `(Option String)` and the pass-by-pointer `(Result String int)`
  param cases end to end -- an inline-C `tur_some_ptr` / `tur_none` /
  `tur_ok_ptr` / `tur_err_int` result passed straight into a user function
  typed on the aggregate.

**Note on the Bucket B accessors.** The owned-String optional accessors that
originally motivated this report ship in `stdlib/httpd-string.tur`
(`httpd-req-cookie-opt` / `httpd-req-form-opt`) and `stdlib/range-bound-string.tur`
via a Turmeric wrapper over a low-level `*-raw` cstr helper -- the workaround
this straddle forced. With the straddle now fixed, those `*-opt` wrappers *may*
be simplified to return `(Option String)` directly from inline-C
(`tur_some_ptr` / `tur_none`); that is an optional follow-up, not required for
correctness -- the current wrapper form is already correct and green.
