---
title: `(unwrap-or (let [o (some N)] (option-map o (fn [x] ...))) D)` -- by-value
  `Option__int` flows into the carrier-`int64_t` `unwrap-or` slot without a
  bridge when the producer sits inside a nested `let` arg
category: Codegen / ABI -- Option none-as-NULL retirement (Track A)
severity: Medium. Hard cc error
  (`incompatible type for argument 1 of 'unwrap_hyor'`). NOT a silent
  miscompile. Regression vs. pre-PR #421 -- the same expression compiled and
  ran (`prints 15`) before option-map was retyped to its pure-Turmeric by-value
  body. Workaround: hoist the `let` so the option-map call is the *direct* arg
  of `unwrap-or` (e.g. `(let [o (some 5)] (println (unwrap-or (option-map o ...)
  99)))`). Severity is "medium" rather than "low" because realistic code
  routinely binds the option in a `let` and consumes the mapped result in an
  outer expression position; the workaround is mechanical but easy to miss.
status: RESOLVED 2026-06-18 via direction 1 (emit-side fix). The arg-slot
  spill check for the carrier-`int` consumer (direct call, `dict_arg ==
  NULL`) in `src/compiler/emit_expr.c` now calls
  `fn_body_tail_emits_byvalue_carrier_abi` instead of
  `expr_emits_byvalue_carrier_abi`, so a by-value `Option__A` producer
  buried in a `let`/`do`/`if`/ascribe wrapper's tail still triggers the
  `&temp + (int64_t)(intptr_t)` spill bridge at the carrier boundary. The
  cascade-coupled long-term retype tracked in
  `docs/reported/option-consumer-retype-byvalue.md` is unaffected and
  remains the right end state. Regression fixture:
  `tests/fixtures/option-map-byvalue-result-into-carrier-consumer-let-inside-arg/`
  (covers the `let` wrapper from the report plus `do` and `if`-arm
  variants).
---

# By-value `Option__A` -> carrier-`int64_t` consumer slot misses a spill bridge inside a nested `let` arg

## Summary

After PR #421 retyped `option-map` to a pure-Turmeric by-value body
(`[A B] [o : (Option A) ^fat f : (fn [A] B)] : (Option B)`), the result of a
ground-`A` call (e.g. `o : (Option int)`) is a by-value `Option__int` struct.
The unretyped `unwrap-or` consumer still takes the carrier `o : int`. When the
option-map call is the **direct** arg of `unwrap-or`, emit spills a `&temp`
bridge and casts to `int64_t`, so the call compiles. When the producer sits
inside a nested form (`let`, `do`, `if` arm) in the consumer's arg position,
the spill bridge is not inserted -- the by-value `Option__int` is passed to
the carrier-`int64_t` slot directly and `cc` rejects the call.

## Minimal repros

Works (producer is the direct arg):

```turmeric
(defn main [] : int
  (let [o (some 5)]
    (println (unwrap-or (option-map o (fn [x] (* x 3))) 99)))   ; prints 15
  0)
```

Fails (producer is wrapped in a `let` inside the arg slot):

```turmeric
(defn main [] : int
  (println (unwrap-or (let [o (some 5)] (option-map o (fn [x] (* x 3)))) 99))
  0)
```

```
/tmp/tur-build/_...tur.c:4376:50: error: incompatible type for argument 1 of 'unwrap_hyor'
 4376 |   printf("%lld\n", (long long)(unwrap_hyor(__t29, INT64_C(99))));
      |                                            ^~~~~
      |                                            Option__int
/tmp/tur-build/_...tur.c:3758:36: note: expected 'int64_t' but argument is of type 'Option__int'
 3758 | static int64_t unwrap_hyor(int64_t o, int64_t dflt) {
      |                            ~~~~~~~~^
tur: cc invocation failed (status 256)
```

The two expressions are semantically identical; only the syntactic shape of
the arg differs.

## Pre-PR #421 behaviour

Checked out at `9c22279~1` (the pre-retype parent), the failing repro
compiles and runs:

```sh
$ git checkout 9c22279~1 -- src/ stdlib/ && cmake --build build -j
$ ./build/tur build /tmp/repro.tur -o /tmp/repro && /tmp/repro
15
```

So this is a regression introduced by the option-map retype, not a
pre-existing limitation that the retype exposed.

## Root cause (suspected)

When the option-map call is the direct arg of `unwrap-or`, the call-site
analysis sees an `expr_emits_byvalue_carrier_abi` mismatch (producer returns
by-value `Option__int`, consumer wants carrier `int64_t`) and inserts the
`&temp` spill + `(int64_t)(intptr_t)` cast.

When the option-map call is buried inside a `let`/`do`/`if` arm, the arg-slot
type-mismatch check operates against the **wrapper** expression's type, not
the tail-position producer's type. The `let`'s result Type was set from its
tail (`Option__int`), but the spill-bridge insertion only fires when the
direct arg is a call (`EX_CALL` -> matched-spec lookup). A `let` arg slot
needs the same bridge applied around the tail's result.

A confirming inspection of the emitted C for the working case shows:

```c
Option__int __t31 = option_map__spec__Option__int_Option__int_int64_t(o, ...);
printf("%lld\n", (long long)(unwrap_hyor((int64_t)(intptr_t)(&__t31), INT64_C(99))));
```

(the `&__t31` spill + cast). The failing case skips the spill, lowering the
expression-statement form's result `__t29` -- typed `Option__int` -- and
calls `unwrap_hyor(__t29, ...)` directly.

## Proposed fix directions

1. **Emit (preferred, minimal):** when matching an arg-slot's expected type
   against a non-call wrapper (`let`, `do`, `if`, ...), recurse into the tail
   producer to detect the by-value-aggregate -> carrier-int64 mismatch, and
   insert the spill (`&temp` + `(int64_t)(intptr_t)`) around the wrapper. The
   working direct-call path already knows how to spill; this generalizes the
   trigger.
2. **Cascade (long-term):** retype `unwrap-or` (and `some?`) to take
   `o : (Option A)` per `docs/reported/option-consumer-retype-byvalue.md`.
   That removes the by-value->carrier bridge requirement entirely at this
   boundary, but requires the cascade into `stdlib/refined.tur` + the
   kleisli Arrow surfaces that the retype report tracks.

Direction 1 closes the regression today without waiting for the cascade;
direction 2 is the right end state.

## Validation

A regression fixture would mirror the working
`tests/fixtures/option-map-literal-none-unannotated-lambda/` shape with the
failing nested-`let` variant. Hold off on landing the fixture until either
fix direction lands, so the suite stays green.

## Related

- [docs/reported/option-consumer-retype-byvalue.md](option-consumer-retype-byvalue.md)
  -- the cascade-coupled `some?`/`unwrap-or` retype (direction 2 above).
- [docs/archive/option-map-literal-none-unannotated-fn-no-A-inference.md](../archive/option-map-literal-none-unannotated-fn-no-A-inference.md)
  -- the report whose validation surfaced this regression; the literal-`(none)`
  + unannotated-lambda repro itself works in HEAD, but the nested-`let` shape
  uncovered here does not.
