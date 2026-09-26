# A `bool` closure called through the int64 carrier can read as true

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
