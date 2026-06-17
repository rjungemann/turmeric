# Pure-Turmeric `defn` returning `(Result A B)` does not bridge when its body is a tail call to an inline-C carrier-ABI helper

**Status:** Reported
**Severity:** Hard compile error. Codegen produces C that fails to compile
(`incompatible types when returning type 'int64_t' but 'Result_A_B' was expected`).
Blocks any thin wrapper around an inline-C helper that returns a Result/Option.
**Discovered:** 2026-06-17, during turmeric-spices `tur-tourist` follow-up work
(Track C, retyping `param` / `capture` ctx parameter to `Ctx`).
**Repro environment:** `tur` built from `main` at `4030562` (post-PR #412);
turmeric-spices at `40ae1c2` (post-PR #8).

---

## Summary

A pure-Turmeric `defn` whose declared return type is an algebraic data type
(here `(Result cstr cstr)`) is lowered with the by-value struct ABI (the
function's C signature is `Result__cstr__cstr (...)`). When its body is a
tail call to an *inline-C* helper that returns the same ADT (lowered with
the carrier `int64_t` ABI), no bridge is inserted at the return: the
generated C body is simply `return inlineC_helper(...);`, which fails to
type-check because the helper returns `int64_t` and the enclosing function
returns a struct.

The same situation works when the *enclosing* function is itself inline-C
(both sides use the carrier ABI), and works when the *callee* is also
pure-Turmeric returning by-value (both sides use the by-value ABI). The
mixed case -- pure-Turmeric wrapper around an inline-C carrier helper --
is what breaks.

## Minimal repro

Equivalent of the broken pair in `tur-tourist`:

```turmeric
;; Inline-C helper; declared return type lowered to carrier int64_t.
(defn captures-get [caps : int key : cstr] : (Result cstr cstr)
  ```c
  return tur_ok((int64_t)(intptr_t)"x");
  ```)

;; Pure-Turmeric wrapper; declared same return type, lowered to by-value struct.
(defn capture [caps : int name : cstr] : (Result cstr cstr)
  (captures-get caps name))
```

`tur build` over the resulting module fails at the C compile step with:

```
error: incompatible types when returning type 'int64_t' {aka 'long int'} but
       'Result__cstr__cstr' was expected
        return tourist__router__captures_hyget(... );
```

(Verbatim line from the spice repro:
`/tmp/tur-build/tests_fixtures_path-capture_path-capture_tur.c:4260`.)

## Observed vs. expected

- **Observed:** the pure-Turmeric wrapper's generated body is a bare
  `return <inlineC_helper>(...)` with no bridge. C compile fails.
- **Expected:** the codegen should insert the carrier->by-value bridge at
  the return site, identical to what it already does at every other
  carrier/by-value boundary (e.g. the bridge already used for non-tail
  callers of `captures-get`).

## Why this is not a `:int` regression test artifact

The audit-driven retype that surfaced this is a one-line parameter change
(`ctx : int` -> `ctx : Ctx`); the body and return type are unchanged. The
build also fails *without* the retype (the bug reproduces on stock `main`
with the spices repo at `40ae1c2`). The retype only made the failure
visible by causing the spice's `path-capture` fixture to be the
representative-build I ran while validating the change.

## Likely root cause

The by-value/carrier bridge logic appears to recognize the boundary at
*non-tail* call sites (a `(let [r (callee ...)] r)` form would produce a
copy through `r` and thus an opportunity to bridge) but skips the
boundary when the callee is the function's tail expression. Specifically,
the codegen path that emits `return <expr>;` for a tail-position call
seems to assume the callee's C return type matches the enclosing
function's, without consulting the per-callee ABI choice (inline-C ->
carrier vs. pure-Turmeric -> by-value).

Plausible fix sites (haven't pinpointed yet; submitting now so it doesn't
get lost):

- The tail-call codegen path in the C backend, when the enclosing
  function's lowered return ABI differs from the callee's.
- Alternatively, the bridge-synthesis pass that already wraps non-tail
  carrier producers consumed by by-value consumers: extend it to cover
  the tail-position case.

## Workarounds (none clean)

1. Force a temporary binding so the bridge fires off the let:
   ```turmeric
   (defn capture [...] : (Result cstr cstr)
     (let [r (captures-get caps name)] r))
   ```
   I have not verified this dodges the bug; it may still take the
   tail-call path after the let collapses.
2. Re-implement the wrapper as inline-C so both sides use the carrier
   ABI. This re-introduces the kind of inline-C the audit is trying to
   retire and loses the by-value ergonomics for downstream callers.
3. Lower the inline-C helper as by-value as well (extra plumbing inside
   each inline-C body to construct the C struct return). Spreads the
   workaround across every inline-C ADT-returning helper.

None of these belong in spice source; this should be fixed in the
compiler bridge.

## Proposed fix directions

Extend the carrier/by-value bridge to fire on tail-position calls:

- At `return <call>;` emission time, if the call's C return type does
  not equal the enclosing function's C return type, emit the same bridge
  the non-tail path already uses (carrier `int64_t` -> by-value struct
  via `Result__A__B { .is_ok = ..., .ok_val = ..., .err_val = ... }`,
  reading the tag and payload back out of the carrier-encoded `int64_t`).
- Mirror logic on the `inline-C carrier <- by-value tail callee` side
  (likely already broken symmetrically, though I have not constructed
  that direction's repro).

## Validation

- New compiled fixture: pure-Turmeric `defn` returning `(Result cstr cstr)`
  whose body is a tail call to an inline-C `defn` returning the same type,
  builds and runs.
- Mirror fixture for `(Option T)`.
- `tur-tourist`'s `path-capture` fixture builds and the existing test
  suite passes against unmodified spice source (the bug is fully behind
  the compiler boundary).

## Cross-references

- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the Track C
  audit; the discovered breakage is on the very thin wrappers the audit
  recommends keeping pure-Turmeric. Fixing this bug unblocks landing the
  `param` / `capture` ctx-retype as a pure spice change.
- `docs/upcoming/end-to-end-monomorphization-plan.md` -- M4-class bridge
  work; this finding likely belongs in the same family of carrier/by-value
  ABI boundary issues that umbrella tracks.
- PR #402 (`Fix carrier-ABI bridge generation for control-form and
  by-value Option returns`) -- the most recent bridge-generation fix on
  `main`; this finding looks like the same family, one direction (tail
  position) over.
