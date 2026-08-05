---
status: open
severity: medium
discovered: 2026-07-29
area: compiler (effect handler emission, src/compiler/emit_effects.c)
---

# `set!` of an enclosing `^mut` binding inside a handler clause emits invalid C

## Summary

A `handle` clause that assigns to a `^mut` binding from the enclosing function
generates C that references the variable without declaring it. The clause is
emitted as its own function (`main_hc0_1`), and the enclosing binding is neither
captured nor passed in, so `cc` rejects the output:

    error: 's_1301' undeclared (first use in this function)

This is the textbook way to write a `State` handler -- bind the state in a
`let`, read it in the `Get` clause, `set!` it in the `Put` clause -- so it is the
first thing anyone writing a state effect tries. It was the shape the old
`effects-vs-monads.md` state example used, which is how it surfaced.

## Repro

    $ cat > /tmp/s.tur <<'EOF'
    (defeffect Get []        : int)
    (defeffect Put [v : int] : nil)

    (defn counter-step [] #fx{Get Put} : nil
      (perform (Put (+ (perform (Get)) 1))))

    (defn main [] : int
      (let [^mut s 0]
        (handle (do (counter-step) (counter-step) (counter-step))
          (Get []  k) (resume k s)
          (Put [v] k) (do (set! s v) (resume k nil)))
        (println s))
      0)
    EOF
    $ ./build/tur run /tmp/s.tur
    /tmp/tur-build/s_tur.c: In function 'main_hc0_1':
    /tmp/tur-build/s_tur.c:6945:5: error: 's_1301' undeclared (first use in this function); did you mean 'k_1304'?
     6945 |     s_1301 = v_1303;
    tur: cc invocation failed (status 256)

Expected: prints `3`.

Note the *read* side is fine on its own -- a clause that only reads `s`
(`(Get [] k) (resume k s)`) compiles. It is specifically the assignment that
emits an undeclared reference.

## Root cause

Not pinpointed. The clause lands in a separate emitted function
(`<fn>_hc<N>_<M>`) and the enclosing frame's mutable bindings are not part of
whatever environment that function receives. Compare
`docs/archive/cps-handler-case-consumes-owning-capture-evicts.md` and
`docs/archive/cps-owning-field-borrow-in-handler-case-not-admitted.md`, both of
which were the same "emitted `__effect_handler_*` references `<var>` undeclared"
symptom on the *owning-capture* side and were fixed by admitting the capture.
This looks like the mutable-binding case of the same gap.

The diagnostic side is also missing: the elaborator accepts the `set!` without
complaint, so the failure only appears as a C compile error with a mangled name
in it. Whatever the resolution, a `TUR-E`-level rejection would be better than
that.

## Fix directions

1. Preferred: admit the capture. The clause needs the enclosing frame's mutable
   bindings by reference, the same way the owning-capture fixes above thread
   theirs.
2. Failing that, reject it in the elaborator with a real diagnostic pointing at
   the heap-cell workaround, rather than letting it reach `cc`.
3. Fixture either way: the state-handler shape above is the canonical repro and
   belongs in the effect fixtures regardless of which direction is taken.

## Workaround

Put the state in a heap cell and write through it, which works today:

    (defn cell-new []                      : ptr<void> ```c return calloc(1, sizeof(int64_t)); ```)
    (defn cell-get [c : ptr<void>]         : int       ```c return *(int64_t *)c; ```)
    (defn cell-put [c : ptr<void> v : int] : int       ```c *(int64_t *)c = v; return v; ```)

    (let [s (cell-new)]
      (handle (do (counter-step) (counter-step) (counter-step))
        (Get []  k) (resume k (cell-get s))
        (Put [v] k) (do (cell-put s v) (resume k nil)))
      (println (cell-get s)))            ; => 3

`stdlib/ref.tur` is not usable for this -- it ships `ref-new` / `ref-get` /
`ref-free` but no `ref-set`, so there is no pure-Turmeric mutable cell to reach
for. Adding one would make the workaround presentable.

The state section of `docs/guides/effects-vs-monads.md` documents the heap-cell
form and says why.
