# call/cc escape continuation passed through a `:cont` param mis-lowers `(k v)`

**Severity:** medium (silent wrong result, not a crash, in a niche feature
combination -- passing a `call/cc`/`escape` continuation out of its lexical
scope through a typed parameter).

**Status:** open. Pre-existing; surfaced while validating SR N4 Slice 2
(`call/cc` on the work-stack). Independent of that runtime change -- the defect
is in elaboration.

## Summary

`(k v)` on a `TY_CONT` binding always desugars to a *delimited* cloneable
continuation resume (`tur_cloneable_cont_resume`). But a `call/cc` / `escape`
continuation is an *undelimited upward escape*, not a resumable delimited
continuation. When such a `k` is passed through a `:cont`-typed parameter and
invoked there, it takes the cloneable-resume lowering instead of
`tur_escape_resume`, so the upward escape never happens: the value is returned
normally and the intervening frames are NOT discarded. The escape-boundary
pointer is also reinterpreted as a `TuriCont*` (the wrong struct).

It works correctly only when `k` is invoked where the elaborator still knows it
is an escape boundary -- i.e. lexically inside the `call/cc` lambda (including a
nested closure that captures `k`).

## Minimal repro

```turmeric
;; Correct (k captured lexically): escapes past the +1 frames -> 104
(defn ok [] :int
  (+ 5 (call/cc (fn [k]
    (letrec [loop (fn [n :int] :int
                    (if (= n 0) (k 99) (+ 1 (loop (- n 1)))))]
      (loop 3000))))))            ;; => 104  (5 + 99)

;; Wrong (k passed through a :cont param): no escape, +1 frames accumulate
(defn deep [k :cont n :int] :int
  (if (= n 0) (k 99) (+ 1 (deep k (- n 1)))))
(defn bad [] :int
  (+ 5 (call/cc (fn [k] (deep k 3000)))))   ;; => 3104  (5 + 99 + 3000), expected 104
```

`(println (ok))` prints `104`; `(println (bad))` prints `3104`.

With `k :int` instead of `:cont`, `(k 99)` is rejected outright:
`'k' is not a function or continuation` (elab_call.c:2809) -- so `:cont` is the
only annotation that type-checks, and it mis-behaves.

## Root cause

`src/compiler/elab_call.c:2821` -- the `(k v)` application sugar for a
`fn_type.kind == TY_CONT` binding unconditionally lowers to a cloneable
continuation resume:

```c
if (fn_type.kind == TY_CONT) {
    ... // desugars to tur_cloneable_cont_resume k v
}
```

`TY_CONT` conflates two runtime flavors that need different `(k v)` lowerings:

- cloneable / serial *delimited* continuations (resumable) -> cont-resume
  (`tur_cloneable_cont_resume` / `tur_serial_cont_resume`). Correct here.
- `call/cc` / `escape` *escape* continuations (undelimited, one-shot, upward) ->
  `tur_escape_resume`. This lowering is only chosen when the binding is the
  lexical `call/cc` handle; once the value flows through a `:cont` parameter the
  escape-vs-delimited distinction is lost and it falls into the cont-resume arm.

At runtime the cont-resume path then casts the `TuriEscapeBoundary*` to
`TuriCont*` (`ts_cont_resume`, `src/turi/eval.c`), reading the wrong struct.

## Fix directions

- Distinguish escape continuations from delimited continuations in the type
  system -- e.g. a separate `TY_ESCAPE_CONT` (or a flavor bit on `TY_CONT`)
  carried through parameters, so `(k v)` lowers to `tur_escape_resume` for an
  escape handle wherever it is invoked, not only at the lexical `call/cc` site.
- Until then, document that a `call/cc`/`escape` continuation must be invoked
  within the lexical extent of its `call/cc` (directly or via a capturing
  closure) and must not be passed through a `:cont` parameter.

## Not in scope for

SR (turi-cek-stackless-reentry). That plan moves the escape *runtime* onto the
work-stack; this is an orthogonal elaboration-level type-distinction gap.
