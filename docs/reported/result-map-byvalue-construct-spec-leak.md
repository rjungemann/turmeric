---
title: Retyping `result-map` to by-value leaks a by-value `ok`/`err` `#{Construct}` spec onto unrelated carrier-context call sites
category: Stdlib / ABI -- Option/Result none-as-NULL retirement (Track A, step 1.2)
severity: Medium. Blocks the `result-map` by-value retype (Phase 1.2 of
  end-to-end-monomorphization-plan). The retype itself is a clean one-liner, but
  introducing a pure-Turmeric body that constructs via `(ok ...)` / `(err ...)`
  causes a by-value `ok__spec__Result__int__int` to be interned, and the
  monomorphization recording pass then routes an UNRELATED carrier-context call
  -- `(ok? (ok 1))` in `tests/fixtures/typed/result-basic/` -- to that by-value
  spec, producing a C type error (`int64_t = Result__int__int`).
status: OPEN, retype reverted to keep the suite green (1683/0). `result-map`
  stays on its inline-C carrier body in `stdlib/result.tur`.
---

# `result-map` by-value retype leaks a by-value construct spec

## Repro

Retype `result-map` per the plan:

```turmeric
(defn result-map [A B E] [r : (Result A E) ^fat f : (fn [A] B)] : (Result B E)
  (if (.is-ok r) (ok (f (.ok-val r))) (err (.err-val r))))
```

`bash tests/run.sh` then fails `tests/fixtures/typed/result-basic/` at the C
compile step:

```
error: incompatible type for argument 1 of 'ok_qu'
   return ok_qu(ok__spec__Result__int__int_int64_t(INT64_C(1)));
                ^ Result__int__int   (expected int64_t)
```

`test-ok` is just `(ok? (ok 1))`. `ok?` takes the int64 carrier
(`[r : int]`); `(ok 1)` should lower to the carrier base. But once the
by-value retype of `result-map` interns `ok__spec__Result__int__int` (returning
the `Result__int__int` struct by value), the scan/record pass tags
`test-ok`'s `(ok 1)` with that by-value clone, so `emit_call_name` emits the
struct-returning spec into a carrier-int argument slot.

## Root cause (partial)

Two layers conspire:

1. **`emit_module.c` record pass.** `(ok 1)` in `test-ok` is recorded
   (`emit_abi_record_specialized_call`) against the by-value
   `ok__spec__Result__int__int` once that spec exists -- even though its
   consumer `ok?` wants the carrier. The record is keyed per-`Expr*`, so the
   bug is that the scan decides `needs_byvalue_spec` for `test-ok`'s `(ok 1)`
   only as a side effect of `result-map` interning the spec elsewhere. The
   exact gate that flips (`emit_module.c:1566-1675`) was not pinned down.

2. **`emit_expr.c::find_matched_abi_spec`.** The 0-arg `#{Construct}`
   disambiguation (only the per-`Expr*` recording may select a by-value spec;
   the structural by-args match cannot tell a carrier base from a by-value spec
   that differs only in return ABI) does NOT extend to N-arg constructors. A
   guard `if (fn_binding->is_construct_template) return NULL;` is the natural
   analogue, but it is insufficient on its own because the emission goes through
   `emit_call_name`'s recording (layer 1), not this function. Both layers need
   to agree.

## Why this is novel vs. `option-map` (step 3, landed)

`option-map`'s pure-Turmeric body constructs `(some ...)` / `(none)` and the
0-arg `(none)` disambiguation was solved by the per-`Expr*` recording guard
(`find_matched_abi_spec`, `emit_module.c` structural-match guards). `result-map`
adds the *1-arg* constructor case (`(ok x)` / `(err e)`): a by-value spec and
the carrier base share identical argument types and differ only in return ABI,
so neither the structural by-args match nor the scan's `needs_byvalue_spec`
gate distinguishes a carrier-context `(ok 1)` from a by-value one without
consulting the *consumer's* expected ABI.

## Proposed fix directions

1. **Carrier-context-aware construct spec selection.** Extend the 0-arg
   construct disambiguation to N-arg `#{Construct}` bindings on BOTH the record
   pass (`emit_module.c`: don't tag a construct call's `Expr*` with a by-value
   clone unless its consuming context wants by-value) and
   `find_matched_abi_spec` (`emit_expr.c`: `is_construct_template` constructors
   resolve their spec only from the per-`Expr*` recording). The two must stay in
   lockstep (there is already a comment to that effect in
   `find_matched_abi_spec`).
2. **Retype `ok?` / `err?` to by-value** `[A B] [r : (Result A B)] : bool`
   (they are themselves No-Lazy-`:int` violations -- `r : int` is a carrier
   stand-in for `(Result A B)`). Then `(ok? (ok 1))` is by-value -> by-value
   and never needs the carrier base. This is the more principled fix but
   cascades into `ok?`/`err?`'s callers and their native interpreter overrides.

## Validation for a fix

- The retyped `result-map` (above) lands; `tests/fixtures/typed/result-basic/`,
  `tests/fixtures/result-combinators/`, and the carrier-bridge regression
  (currently `tests/fixtures/typed-slots/coerce-carrier-to-struct/`) stay green.
- A carrier-context `(ok? (ok 1))` still lowers to the carrier base, not a
  by-value spec.
- Full suite green; `result-map` struck from the Remaining list in
  `option-consumer-retype-byvalue.md`.
- The `coerce-carrier-to-struct` fixture (which feeds a bare `:int` carrier into
  `result-map`) is decoupled from `result-map` per plan 1.2: a dedicated
  carrier-bridge regression fixture covers the `(:: <carrier> (Result int int))`
  -> `ok-val` path via an inline-C carrier producer, so retyping `result-map`
  does not erase that coverage.

## Related

- `docs/upcoming/end-to-end-monomorphization-plan.md` Phase 1.2.
- `docs/reported/option-consumer-retype-byvalue.md` (the `option-map` step 3
  precedent for the 0-arg construct disambiguation).
- `src/compiler/emit_module.c` (`emit_abi_record_specialized_call`, the
  `needs_byvalue_spec` gate ~1566-1675).
- `src/compiler/emit_expr.c::find_matched_abi_spec` (the per-`Expr*` vs
  structural-args disambiguation).
- `stdlib/result.tur` (`result-map`, `ok?`/`err?`).
