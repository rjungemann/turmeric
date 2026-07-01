---
title: defdata parametric return-type inference is weak; unascribed match crashes elab_match
severity: MEDIUM. The SEGV is a compiler crash; the inference gap forces boilerplate ascription throughout parametric-defdata code.
status: OPEN. Found 2026-07-01 while writing the pure-Turmeric parser-combinators fixture.
---

# defdata parametric inference gap + elab_match SEGV

There are three linked issues around parametric `defdata` types. All
three surfaced in one session while writing
`tests/fixtures/parsec-tutorial/input.tur` (a pure-Turmeric parser).

## 1. `defdata` requires keyword-prefixed field types; `defgadt` does not

```turmeric
(defdata PRes [a]
  (PFail)
  (POK a int))     ;; error: defdata: constructor field type must be a keyword like :int

(defdata PRes [a]
  (PFail)
  (POK a :int))    ;; OK
```

`defgadt` accepts both `int` and `:int` in the same position, so the
form-to-form inconsistency is real.

Fix: teach the `defdata` parser to accept either `int` or `:int` (delegate
to whatever `defgadt` already does).

## 2. Parametric `defdata` return-type inference is weak

Given a `(defdata PRes [a] (PFail) (POK a :int))` and a user function
returning `(PRes Expr)`, bare constructor calls like `(PFail)` or
`(POK ast rest)` do not infer to `(PRes Expr)` in every branch of a
nested match. Every branch has to be explicitly ascribed:

```turmeric
(defn factor [xs : int] : (PRes Expr)
  (if (at-end? xs)
    (:: (PFail) (PRes Expr))               ;; ascription required
    ...
      (match (expr-parse (list-tail xs))
        (PFail)
        (:: (PFail) (PRes Expr))           ;; ascription required
        (POK inner rest)
        (:: (POK inner (list-tail rest))   ;; ascription required
            (PRes Expr))
        ...)))
```

Without the ascription the elaborator either reports a spurious
"cannot unify (PRes int) with (PRes Expr)" or -- see #3 -- crashes.

## 3. `elab_match` SEGV on unascribed parametric match

While iterating on the fixture, several unascribed nested-match
combinations triggered a segmentation fault in the elaborator. The
crash site during a `lldb` catch was inside `elab_structs.c` near
line 3990 (`elab_match`). Reducing the failing snippet to a minimal
repro turned out to be finicky -- ascribing every branch made the
crash go away -- and I did not narrow it further before pivoting.

Reproducing minimally (approximate; the exact shape mattered):

```turmeric
(defdata PRes [a]
  (PFail)
  (POK a :int))

(defn go [xs : int] : (PRes int)
  (match (parse xs)
    (PFail)                    (PFail)     ;; unascribed bare (PFail) here
    (POK n rest)               (POK n rest)))
```

The elaborator should either accept this (per #2) or emit a proper
diagnostic. A SEGV is never right.

## Impact

Writing pure-Turmeric code that uses parametric `defdata` sum types
(the natural Turmeric idiom for parser/decoder results, tagged unions,
etc.) currently requires per-branch `(:: expr (Foo T))` ascription
everywhere, and one wrong ascription can crash the compiler.

## Fix directions

- **#1** should be a small parser change in whichever `defdata` form
  reader rejects bare type names.
- **#2** likely wants the same top-down return-type propagation that
  makes `defgadt` matches infer their result correctly. Worth
  comparing the two elaboration paths side-by-side.
- **#3**: whatever fix lands for #2 will probably also close #3, but
  either way the elaborator should never SEGV -- add a guard or an
  `elab_assert` at the crash site as a safety net.

## Workaround (in use)

`tests/fixtures/parsec-tutorial/input.tur` ascribes every
`(PFail)` / `(POK ...)` constructor call with `(:: ... (PRes Expr))`.
It compiles and prints `15`; the boilerplate is the only cost.
