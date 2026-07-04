---
title: By-value `Option`/`Result` flowing through an `if` join with a
  function-call arm is cast aggregate->pointer in codegen
severity: MEDIUM. Codegen defect. A well-typed program (`tur check` passes)
  fails to compile the emitted C with "aggregate value used where an integer
  was expected". Blocks a natural `(if c (some ..) (returns-option ..))`
  join pattern; a workaround exists (use direct constructors in both arms, or
  bind through a typed helper), so it does not block the one track.
status: OPEN. Filed 2026-07-04 while landing the return-directed-methods
  if-sibling inference fix (docs/reported/return-directed-methods-pure-empty-
  inference.md). Independent of that fix -- the same failure reproduces with a
  plain `some` arm and no return-directed method involved.
---

# By-value Option through an `if` join with a function-call arm miscompiles

## Symptom

A well-typed `if` whose result type is a by-value `(Option int)` and one of
whose arms is a *function call* returning that Option emits C that casts the
call's aggregate return value to a pointer:

```turmeric
(defn opt-val [o : (Option int)] : int (if (some? o) (.value o) -1))
(defn known [] : (Option int) (some 5))

(defn pick [b : bool] : int
  (opt-val (if b (some 99) (known))))          ; known() is a call arm

(defn main [] : int (println (pick true)) 0)
```

`tur check` passes, but `tur run` / `tur build` fails at the C compiler:

```
error: aggregate value used where an integer was expected
    tur_option_t *__t57 = (tur_option_t *)(intptr_t)(known());
                          ^~~~~~~~~~~~
```

## Scope / what does and does not trip it

- Trips it: an arm that is a **call returning a by-value Option/Result**
  (`(known)` above). Reproduces with a plain `(some ..)` in the sibling arm --
  no typeclass method or return-directed dispatch is involved.
- Does **not** trip it: both arms being direct constructors, e.g.
  `(if b (some 99) (some 5))` or `(if b (some 5) (pure 99))`, compile and run
  fine. Binding the `if` in a `let` does not change the outcome either way; the
  determining factor is the function-call arm.

## Root cause (direction)

The `if`-join emit path materialises the branch value through the `int64`
carrier (`(tur_option_t *)(intptr_t)(...)`), which is correct for a carrier
handle but wrong for a by-value aggregate return: `known()` already yields a
`tur_option_t` by value, so the `(T*)(intptr_t)` carrier cast is applied to a
struct rvalue. The direct-constructor arms avoid it because the constructor is
lowered straight into the aggregate temporary. This is the same family as
`docs/archive/history/byvalue-result-field-access-casts-aggregate-to-pointer.md`
(aggregate-vs-carrier confusion at a by-value boundary), here at the if-join
temporary rather than a field access.

Likely fix site: the branch-result coercion in the `EX_IF` emit path
(`src/compiler/emit_*.c`) should skip the carrier `(T*)(intptr_t)` cast when the
branch expression's type is a by-value aggregate register class.

## Workaround

Use direct constructors in both arms, or route the call arm's Option through a
typed accessor before the join.
