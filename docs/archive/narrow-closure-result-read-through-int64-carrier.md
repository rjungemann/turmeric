# A `bool` closure called through the int64 carrier can read as true

> **RESOLVED 2026-09-26.** See [Resolution](#resolution) at the end. The
> analysis below is the original filing.

**Severity:** high -- a silent wrong answer on the compiled path; `tur
--interpret` is correct. Found 2026-09-25 while testing `Applicative [(Result _
B)]`; reproduces on the compiler before that change through `Result`'s existing
`fmap`.

## Repro

```turmeric
(defn main [] : int
  (let [a3   (:: (ok 3) (Result int cstr))
        big? (fn [x : int] : bool (> x 10))]
    (println (match (fmap a3 big?) (Ok v) (if v "ok true" "ok false") (Err e) e)))
  0)
```

```
$ tur run repro.tur          # ok true   (wrong: 3 > 10 is false)
$ tur --interpret repro.tur  # ok false
```

The same happens through `ap` on a `(Result (fn [int] bool) cstr)`. The
mixed-type receiver matters: it keeps the call on the erased carrier path
rather than a by-value spec. `Option`'s `ap` with the same function is correct,
because its by-value spec calls the function through its typed signature.

## Cause

The lambda is `static bool __fn_N(int64_t x)`, and its fat box's slot 0 holds
the typed shim `static bool __tur_fatshim_bool_int64_t(void *, int64_t)`. The
instance's carrier body does not know the element's type, so it calls slot 0 as
`int64_t (*)(void *, int64_t)` and reads the whole return register. A `bool`
return defines only the low byte (AL on x86-64); the upper bits are whatever the
callee left there, so a false result can read as non-zero.

A capturing closure has the same exposure through its own thunk, declared with
the real `bool` return. It printed correctly in the one probe run, which is
luck of register contents, not a different rule.

## Fix directions

The erased caller cannot know the width, so the callee side has to widen. Every
closure entry point that an erased caller can reach through slot 0 -- the typed
fatshims and the capturing-closure thunks -- could return a narrow integer
result (`bool`, `int8`/`int16`/`int32`, the unsigned widths) zero- or
sign-extended as `int64_t`. A typed caller that casts slot 0 to the narrow
signature still reads the low byte correctly, so both callers agree.
`float`/`float32` results have the analogous problem in a different register
and are out of scope for the integer fix.

## Resolution

Fixed on the callee side, as the report proposed, with typed callers moved in
step so both kinds of caller agree with every entry point.

- **Erased sinks.** Where a function value is converted for a sink whose
  declared result is erased -- a carrier-base instance's `g : (fn [a] b)` --
  the float carrier shims already bridged a float result through its bits. An
  erased narrow integer result is now widened to `int64_t` by a C conversion
  in the same shims (`narrow_int_carrier` in `src/compiler/emit_module.c`),
  and the bare-function adapter (`ensure_bare_fnptr_poly_shim`) takes the same
  flag. This covers `fmap` with a let-bound lambda, a capturing closure and a
  top-level function.
- **Slot 0.** A function stored in a container and applied by an erased
  instance (`ap` on a `(Result (fn [int] bool) cstr)`) reaches slot 0 with no
  conversion site in between. Slot 0 now returns a narrow result widened
  (`thunk_result_slot_c_name`):
  - the typed fatshim in a boxed bare function's slot 0 widens;
  - the typed poly-to-fat shim widens;
  - a capturing closure's slot 0 holds a widening wrapper
    (`ensure_closure_slot0_widen`) that calls the thunk through the typed
    pointer callers used before, so the thunk's own signature, spelled in
    several places, is unchanged.
- **Typed callers.** The typed-thunk typedef returns `int64_t` for a narrow
  result, and the hand-built typed slot-0 casts (the boxed fn-field call, the
  two fat dispatches, the async spawn wrapper) call through `int64_t` and
  convert back to the declared type.

Not changed: a `tur_poly_fn_t`'s `.fn` still returns the declared type, and
typed readers of it and the `TUR_APPLYn_T` macros in stdlib inline C read the
narrow type. On the native ABIs a narrow read of a widened return is the same
value, so those stay correct. Floats were already handled.

Pinned by `tests/fixtures/narrow-closure-result-through-carrier` (every shape
above, plus `int8`, `uint16` and `int32` results) under both harnesses. The
155 regenerated snapshots change only the stdlib `bool` comparator's typedef,
shim and typed call sites.
