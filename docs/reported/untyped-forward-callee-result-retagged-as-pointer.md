# An untyped defn called before its definition has its `any` result re-tagged as a pointer

**Severity: low-medium.** The result is a C compile error, not a wrong answer,
so nothing runs miscompiled. But it depends on which file is the entry: the
same prelude compiles when a `#lang r7rs` program is the entry, and fails
when a Turmeric module is the entry and imports a Scheme `define-library`.

Found landing r7rs-lang-plan T1, where `tests/run-r7rs-import.sh`'s
`turmeric-imports-scheme` case (compiled) went red. It was worked around by
declaring `: any` on `r7rs-exact` and the other numeric procedures T1 changed.

## Repro

1. In `stdlib/r7rs/prelude.tur`, drop the `: any` from
   `(defn r7rs-exact [x] : any`.
2. Its body returns `x` on one branch. On the others it returns a `let`
   whose `if` yields `(r7rs-big-norm__ ...)` (declared `: any`) or
   `(:: (float->int f) any)`.
3. `r7rs-exint-of__` is defined EARLIER in the file and calls it.
4. Run the `turmeric-imports-scheme` case of `tests/run-r7rs-import.sh`: a
   Turmeric `tmain.tur` that imports a Scheme library, under `tur run`. It
   fails at `cc`:

```
error: aggregate value used where an integer was expected
    return TUR_TAG(3, (int64_t)(intptr_t)(__ps_580));
```

## What the C shows

- The callee is emitted correctly: `static tur_tagged_t r7rs_hyexact(tur_tagged_t)`.
- The caller, `r7rs-exint-of__ : any`, is also correct in its own type.
- The call site wraps the callee's result in `TUR_TAG(3, ...)`, as though the
  result were a pointer-typed value being widened to `any`. So the forward
  reference to an untyped defn resolved to a non-`any` return type,
  presumably one inferred before the defn's body was seen.
- The same prelude compiles when a `#lang r7rs` file is the entry.

## Fix directions

- Find where a forward call to an untyped defn takes its return type (the
  forward-declaration pass in the elaborator). Make it agree with the type
  the defn is finally emitted with: `any`, when the branches widen to `any`.
  Or re-check the call once the body is known.
- Find why a Turmeric entry takes a different path from a Scheme entry.
  Probably the dynamic-substrate flag (`lang_span_is_dynamic` /
  `g_opt_dynamic_any`) is consulted per entry, not per span.
- A regression test: a Saffron module with an untyped defn defined after its
  caller, returning `any` through a `let`, imported from a Turmeric entry.
