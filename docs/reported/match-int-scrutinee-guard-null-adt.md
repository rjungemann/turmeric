# `match` on a non-ADT scrutinee whose first arm is a wildcard dereferences a null `AdtDef`

**Severity:** medium -- UBSan-caught null dereference in codegen. Not a
miscompile in the shipped Release build (no sanitizer), but it is undefined
behaviour on a Debug build and it aborts any ASan/UBSan run that reaches it.

**Found by:** adding a `match`-generating shape to `tests/refine-fuzz-src.py`.
Unrelated to refinement types -- it reproduces with no `#refine{...}` anywhere
and with the `refined` gate off.

## Repro

```turmeric
(defn target [p : int] : int
  (match p _ 0))

(defn main [] : int (println (target 3)) 0)
```

```
$ ./build/tur check repro.tur
src/compiler/emit_expr.c:9334:33: runtime error: member access within null
pointer of type 'struct AdtDef'
```

The scrutinee is an `:int`, so there is no `AdtDef` to name.

## Which shapes are affected

It is decided by the FIRST arm's pattern, not by guards -- my first reading
blamed `when`, which was wrong. A leading wildcard or bare binder takes the ADT
path; a leading literal does not:

```turmeric
(match p _ 0)                     ; UB
(match p _ 0 _ 1)                 ; UB
(match p x x)                     ; UB
(match p 0 0)                     ; ok
(match p 0 0 _ 1)                 ; ok -- literal first, wildcard later is fine
(match p 0 when (>= p 0) 0 _ 1)   ; ok
```

So a wildcard/binder arm is fine anywhere except in first position, which
suggests the dispatch decides "this is an ADT match" from arm 0 alone.

## Root cause

`src/compiler/emit_expr.c:9334` reads `adt->name` to build the ADT's C struct
name:

```c
char *_mn = mangle_field_name(adt->name);
snprintf(adt_c_name, sizeof(adt_c_name), "tur_adt_%s", _mn);
```

`adt` is NULL whenever the scrutinee is not an ADT.

## Fix directions

Guard the block on `adt != NULL` and take the scalar path for a non-ADT
scrutinee, the same way a leading literal arm already does. The fix is probably
one condition; the work is finding which of the `scrut_is_app_monomorph` /
carrier branches below it also assume `adt` is non-NULL. Better still, fix the
dispatch that classifies the match from arm 0, so the decision does not depend
on which arm happens to come first.

## Status

Not fixed. Noticed while adding fuzzer coverage for match-arm hypotheses; the
affected rungs were dropped from the generator (`shape_datatype`) so the fuzzer
does not spend every datatype case on this one crash. Restore the leading-`_`
rungs once this is fixed.

---

## Second defect, same area: guarded wildcard arm emits invalid C

Found the same way (fuzzer/fixture work on match arms), also nothing to do with
refinements -- it reproduces with the gate off and no `#refine{...}` present.

```turmeric
(defn via-guard [x : int] : int
  (match x
    0 0
    _ when (not= x 0) (/ 10 x)
    _ 0))

(defn main [] : int (println (via-guard 5)) 0)
```

```
$ ./build/tur build repro.tur
.../repro.c:7121:9: error: 'else' without a previous 'if'
```

The emitted C for the guarded wildcard arm opens an `else` with no preceding
`if`. A guarded LITERAL arm (`0 when (>= x 0) 0`) is fine, and an unguarded
wildcard in non-first position is fine; it is specifically a wildcard arm
CARRYING a guard, in non-first position, on a non-ADT scrutinee.

Same root area as the null deref above -- the non-ADT match lowering does not
handle wildcard-plus-guard -- so the two are probably one fix. Severity is
higher than the null deref in one respect: this one fails the C compile
outright, so it is a hard "cannot build this program" rather than UB.

Affected fixture rungs are avoided rather than fixed:
`tests/fixtures/refine-crossing-path-conditions` uses a guarded arm on an ADT
scrutinee instead.
