## Option consumers (`some?`, `unwrap-or`, `option-map`, `option-free`) typed `o : int` reject a by-value `(Option A)`

**Status:** Reported (2026-06-17). Split out from
`option-lowering-mid-migration.md` while fixing the *constructor*/
control-form half of that report.

**Severity:** Hard `cc` error (not a miscompile). Blocks feeding a
function that returns `(Option A)` into the stdlib Option predicates/
combinators. Also a `CLAUDE.md` "No Lazy `:int` Stand-Ins" violation:
these parameters are an `Option` handle, typed as `:int`.

## Summary

`stdlib/option.tur` declares several consumers with an `:int` parameter
that is really the carrier-ABI `Option` handle:

```turmeric
(defn some?       [o : int] : bool ...)
(defn unwrap-or   [o : int dflt : int] : int ...)
(defn option-free [o : int] : void ...)
(defn option-map  [o : int ^fat f] : int ...)
```

These compile fine when the argument is itself a carrier int64 (the
historical representation). But a function whose declared return type is
`(Option A)` now lowers to the **by-value** `Option__A` struct (the new
lowering, consistent end to end after the
`option-lowering-mid-migration` fix). Passing such a by-value result to
an `:int` carrier parameter is an incompatible-type error.

## Observed

```turmeric
(defn g [] : (Option int) (some 5))
(defn main [] : int (if (some? (g)) 1 0))
```

```
error: incompatible type for argument 1 of 'some_qu'
        if (some_qu(g())) { ...
            ^~~
note: expected 'int64_t' but argument is of type 'Option__int'
```

The same shape reproduces for `unwrap-or`, `option-map`, `option-free`
and any other `o : int` Option consumer.

## Expected

A by-value `(Option A)` value flows into the Option consumers without a
`cc` error -- either by typing the parameter honestly as `o : (Option A)`
(preferred; lets the by-value/carrier bridge fire at the call site like
it does for `.value` / `.is-some` field access), or by an explicit
caller-side bridge.

## Root cause -- probable

The parameters were authored against the old heap-pointer Option ABI,
where every `(Option A)` value was an int64 carrier. They were never
migrated when concrete `(Option A)` return types moved to the by-value
`Option__A` struct lowering. The call-site arg coercion
(`expr_emits_byvalue_carrier_abi` / the carrier bridge) only knows to
bridge *into* a carrier param when the source is recognized as by-value,
and the sink param's `:int` type erases the fact that it wants the
carrier form of an `Option`.

## Proposed fix directions

1. **Retype the consumers** `o : (Option A)` (and add the `[A]` type
   param), letting the existing concrete->carrier bridge convert a
   by-value argument at the call site -- the same machinery that already
   lets `(.is-some (g))` work on a by-value `g()` result. This is the
   `No Lazy :int Stand-Ins`-compliant direction.
2. Alternatively, recognize an `:int`-typed Option-carrier sink and emit
   the concrete->carrier bridge for a by-value argument. Less honest;
   keeps the `:int` API the type checker cannot help anyone use.

Validate with the `(some? (g))` repro above plus `unwrap-or` /
`option-map` over a by-value-returning producer; run `bash tests/run.sh`
(expect snapshot churn in the Option consumers).

## Cross-references

- `docs/archive/option-lowering-mid-migration.md` (or
  `docs/reported/` until archived) -- the constructor/control-form half,
  now fixed; this is the consumer half of the same migration.
- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- same
  `:int`-as-handle family of findings.

## Resolution (2026-06-17)

Fixed via **direction #2** (call-site concrete->carrier bridge), after
empirically ruling out direction #1.

Direction #1 (retype the consumers `o : (Option A)`) was attempted and
**reverted**: it breaks every carrier-`int` caller. elab's carrier
equivalence is *one-directional* -- it accepts an `(Option int)` value
where an `int` carrier is expected (the bug this report is about), but
rejects an `int` carrier where `(Option A)` is expected
(`TUR-E0001: expected (type-app Option tyvar 'A'), got int`). The stdlib
is full of the latter: `option-map` / `ne-from?` / `bidx-of?` and the
whole `seq` Option machinery return `: int` carriers and feed them into
`some?` / `unwrap-or`. Retyping just `some?` already broke
`(some? (option-map ...))` at elab. Honest-typing the consumers would
therefore require migrating that entire carrier-Option subsystem -- a
much larger effort than this report's scope, tracked with the other
`:int`-stand-in rows in
`docs/reported/spices-int-stand-in-audit-2026-06-14.md`.

Direction #2 is the minimal, blast-radius-free fix and is exactly
symmetric with the bridges already in the emitter (the carrier->concrete
return/field bridges added for `option-lowering-mid-migration`, and the
`dict_arg != NULL` concrete->carrier dispatch bridge):

- **`emit_expr.c` (call-arg coercion)**: a new branch, sibling to the
  existing dict-dispatch concrete->carrier bridge, fires on an ordinary
  *direct* call (`dict_arg == NULL`, no matched ABI spec) when the
  argument genuinely emits a by-value carrier-ABI aggregate
  (`expr_emits_byvalue_carrier_abi`) and the callee declares that slot as
  the int64 carrier (`arg_kinds[i] == TY_INT`). It spills the by-value
  `Option__A` / `Result__A__B` to a temp and passes its address as the
  carrier handle. The `Option__int { bool is_some; int64_t value; }`
  layout matches the canonical `tur_option_t`, so the `:int`-sink impl's
  `tur_is_some` / `tur_opt_value` / `opt->value` reads land correctly;
  `none` ({is_some=false}) reads back false. A bare carrier producer
  (`(some 1)`, `(option-map ...)` returning the carrier) is *not*
  by-value, so the guard leaves it untouched -- it flows as the carrier
  int64 it already is.

Regression fixture: `tests/fixtures/option-consumers-byvalue-arg/`
passes a by-value `(Option int)` into `some?`, `unwrap-or`, `option-map`,
`option-eq?` (and confirms bare carrier producers still flow unchanged).
The change is purely additive -- it only emits a bridge where there was
previously a hard `cc` error -- so there is **zero** snapshot churn
(suite: 1656 passed, 0 failed).

### Still open

The `:int` parameter types themselves are unchanged -- the honest
`o : (Option A)` retype remains blocked on migrating the carrier-`int`
Option producers (`option-map`, `ne-from?`, `bidx-of?`, `seq/*`) to
return `(Option A)` first. That migration is the proper home for
direction #1 and is left to the `spices-int-stand-in-audit` follow-up
rather than forced here, where it would regress a large swath of the
stdlib.
