---
title: Calling `ok?` (or another Result predicate/accessor) on a by-value
  `(Result A B)` parameter miscompiles the parameter materialization
severity: MEDIUM. Codegen defect. A well-typed program (`tur check` passes)
  fails to compile the emitted C: the by-value Result parameter is copied into a
  temp declared as the aggregate but initialized from the int64 carrier
  (`tur_adt_Result__int__cstr __t = r;` where `r` is `int64_t`), then re-cast
  through the carrier. The Option analogue (`some?` on an `(Option A)` param)
  does NOT trip it, so it blocks a natural Result consumer shape while Option
  works.
status: OPEN. Filed 2026-07-04 while landing the if-join aggregate-cast fix
  (docs/archive/byvalue-option-if-join-function-call-arm-aggregate-cast.md).
  Independent of and pre-existing that fix -- it reproduces with no `if` join of
  calls, and on the pre-fix compiler.
---

# By-value Result parameter + `ok?` predicate miscompiles

## Symptom

```turmeric
(defn f [r : (Result int cstr)] : int (if (ok? r) 1 0))
(defn main [] : int (println (f (:: (ok 3) (Result int cstr)))) 0)
```

`tur check` passes; `tur run` / `tur build` fails at the C compiler:

```
error: invalid initializer
    tur_adt_Result__int__cstr __t56 = r;
                                      ^
error: invalid type argument of unary '*' (have 'long int')
    if (ok_qu((*((int64_t)(intptr_t)(&__t56))))) {
               ^
```

The emitted `f` copies the parameter `r` into a by-value aggregate temp
`__t56`, but `r` is held in C as the `int64_t` carrier -- so the initializer is
`aggregate = int64_t` (rejected), and the follow-on `ok?` call then re-casts
`&__t56` back through the carrier.

## Scope

- Trips it: a **by-value `(Result A B)` parameter** consumed by `ok?` (and,
  independently, by `ok-val` / `err?` / `err-val`) inside the function body.
- Does **not** trip it: the Option analogue
  `(defn f [o : (Option int)] : int (if (some? o) 1 0))` compiles and runs. The
  divergence is Result-specific (the 3-field `{is_ok, ok_val, err_val}` carrier
  layout vs Option's 2-field `{is_some, value}`).

## Root cause (direction)

The parameter-materialization seam for a carrier-ABI Result param disagrees on
representation: the temp is declared as the by-value monomorph
`tur_adt_Result__int__cstr` but the source `r` is the int64 carrier, and the
predicate call wants the carrier pointer. Either the temp should be the carrier
(`int64_t`, matching `r`) or `r` should be materialized into the aggregate via
the canonical carrier->concrete unbox (`emit_carrier_bridge`), not a bare
`= r`. The Option path picks a consistent representation; the Result path (wider
carrier record) does not. Likely the same aggregate-vs-carrier family as
`docs/archive/history/byvalue-result-field-access-casts-aggregate-to-pointer.md`.

## Workaround

Extract the Result via an inline-C accessor over `&r` (reading the
`{is_ok, ok_val, err_val}` layout directly), or route the value through the
carrier before applying the predicate.
