# A `^Class`-constrained parameter erases an ADT argument to `int64_t`

**Severity: medium** (emitted C does not compile -- a hard `cc` failure at build
time, not a wrong answer at runtime; the diagnostic points at generated C, not
at the user's source).

**Status:** OPEN. Filed 2026-08-31 while verifying the code samples on
`web/tour/index.html` against the compiler. Found on macOS/arm64 with
`./build/tur` at v0.42.1.

## Summary

When a typeclass-constrained parameter (`^Show a x`) is instantiated at an ADT,
the constrained parameter is emitted as `int64_t` while the generated instance
method takes the ADT struct by value. The call in the emitted C then passes an
`int64_t` where a `struct tur_adt_Color` is expected and `cc` rejects it.

`tur check` is silent -- the mismatch only appears once the C is compiled, so
the first sign of it is a compiler error inside a generated file.

## Repro

```turmeric
(defclass Show [a]
  (show [x] : cstr))

(defdata Color (Red) (Green) (Blue))

(definstance Show [Color]
  (show [c]
    (match c
      (Red)   "red"
      (Green) "green"
      (Blue)  "blue")))

(defn display [^Show a x] : void
  (println (show x)))

(defn main [] : int
  (display (Red))
  (display (Blue))
  0)
```

`tur run repro.tur`:

```
...repro.c:7608:57: error: passing 'int64_t' (aka 'long long') to parameter of
    incompatible type 'tur_adt_Color' (aka 'struct tur_adt_Color')
 7608 |         const char * __ps_173 = (__inst_Show_show_Color(x));
      |                                                         ^
...repro.c:4343:58: note: passing argument to parameter 'c' here
 4343 | static const char * __inst_Show_show_Color(tur_adt_Color c) {
```

Reproduces identically in sweet-exp, so it is not a reader-level problem.

## What narrows it

- A constrained parameter instantiated at `int` works -- the erasure is a no-op
  there, which is why the pattern looks fine in the common case.
- The instance body itself is fine: `__inst_Show_show_Color` is emitted with the
  correct by-value ADT signature. Only the *call site* inside the constrained
  function disagrees about the representation.

## Root cause (suspected)

This is the hybrid carrier/by-value split described in
[docs/archive/history/end-to-end-monomorphization-plan.md](../archive/history/end-to-end-monomorphization-plan.md):
the constrained parameter stays on the `int64_t` carrier while `defdata`
constructors have moved to by-value structs, and the dispatch call does not
insert the unbox. Monomorphizing the constrained function per instance would
remove the mismatch rather than patch the coercion.

## Where it bit

`web/tour/index.html` stop 02 (Typeclasses) shows exactly this program. The
sample was never compilable; it is illustrative markup on the site, so no page
change was made for this report.
