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
