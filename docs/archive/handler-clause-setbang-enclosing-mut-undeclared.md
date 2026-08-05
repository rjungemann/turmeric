---
status: resolved 2026-08-05 (B7b: promote a clause-touched `^mut` to a shared typed cell)
severity: high (as filed: medium -- the read side turned out to be a SILENT wrong answer, not merely "fine")
discovered: 2026-07-29
area: compiler (CPS/DK backend, src/compiler/emit_cps_ir.c -- NOT emit_effects.c as originally filed)
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

> **Correction (2026-08-05, while fixing).** The read side is NOT fine -- it
> merely *compiles*. The capture is by value and the env is populated when the
> handle is INSTALLED, so a clause reading `s` sees the install-time snapshot,
> not the current value. Any write between install and the clause running is
> invisible to it:
>
>     (let [^mut b 5]
>       (println (handle (do (set! b 7) (ping))
>                  (Note [] k) (resume k b))))
>
> prints **5** compiled, **7** under `tur --interpret` (and 7 is right). That is
> a silent wrong answer with no diagnostic at all -- strictly worse than the
> reported `cc` error, and the reason this is filed at high severity in
> retrospect. Both directions have the same root cause and are fixed together.

## Root cause

**Pinned 2026-08-05.** The report guessed `emit_effects.c` (the fiber emitter);
the emitted name `main_hc0_1` is in fact the **CPS/DK backend**'s clause naming
(`<fn>_hc<N>_<M>`, `emit_cps_ir.c`), which is where the whole defect lives.

A clause is lifted into its own C function, and an enclosing binding reaches it
only as a **by-value** field of the capture env struct. So the enclosing frame,
each clause, and the handle continuation each hold a SEPARATE copy of `s`:

- a `set!` in a clause names a variable that function never declared -- the
  reported `cc` error (`collect_free_vars` never surfaces an assignment TARGET,
  so `s` was not even collected as a capture);
- a read gets the copy snapshotted at handle-install time -- the silent stale
  read above;
- and even with both of those fixed in place, `(println s)` after the `handle`
  would still print its own install-time copy.

The machinery to fix it already existed but was keyed too narrowly: B7
(`g_byref_muts`) promotes a `^mut` to a **shared heap cell** whose C name binds
the cell pointer, with reads/writes dereferencing and the pointer riding the
ordinary scalar-capture path. It fired only for `(set! m k)` where the stored
value `is_continuation` -- the escaping-continuation case -- so an ordinary
state mutable never qualified.

The diagnostic side is also missing: the elaborator accepts the `set!` without
complaint, so the failure only appears as a C compile error with a mangled name
in it. Whatever the resolution, a `TUR-E`-level rejection would be better than
that.

## Fix (direction 1 -- admit the capture)

Widen the existing B7 promotion instead of adding a parallel mechanism, and
make the two spellings of a promoted mutable single-chokepoint:

1. **Promote on touch, not just on continuation-store.** `case_mut_scan` walks
   each clause body and promotes every ENCLOSING `^mut` it references -- read or
   written (a clause-local mutable is bound within the body and stays an
   ordinary local). This fixes both directions at once: with one shared cell,
   every view in the function agrees.
2. **Collect the write as a capture.** `collect_caps_rec`'s `CT_LETRAW` arm now
   keys on the promotion (`is_byref_mut`) rather than on the continuation-store
   shape, so the cell pointer rides the clause's env.
3. **Type the cell.** The cell was hard-coded `int64_t *`. That silently
   TRUNCATES a `^mut` float at every write -- `7.1 + 1.5 + 1.5` reads back `9`,
   not `10.1` -- and puns a `cstr` through an integer. It now carries the
   binding's own C type (`byref_cell_ctype`), which also keeps the B7
   continuation case (an int-typed mutable) byte-identical.
4. **One chokepoint per direction.** A read derefs in `atom_var` (emit_core.c),
   which the file already documents as *the* value-position chokepoint -- so a
   delegated read through the direct emitter derefs too, not only a CPS-IR atom.
   A write derefs in `emit_set_stmt` (emit_stmt.c), the store counterpart, which
   is what makes a `set!` nested inside a DELEGATED composite (a `while` in a
   clause) correct -- that store never reaches the CPS emitter's own lowering
   and previously wrote through the raw pointer and segfaulted. Neither deref
   belongs in `name_for_binding`, which must stay bare because it also spells
   the declaration.
5. **Exclude loop-carried mutables.** The native while-loop lowering owns the
   representation of the variables it carries (it threads each as a parameter of
   the emitted loop helper), so promoting one makes the two spellings disagree
   inside the helper. Excluding them costs nothing: a loop-carried mutable READ
   by a clause is already correct by value, because the handle is re-installed
   each iteration, so the snapshot IS the current value.
   (`tests/fixtures/cps-tramp-resume-while-readset` is exactly this shape and
   caught the clash.)

**Known remaining limitation, unchanged by this fix:** a clause that WRITES a
mutable carried by a `while` loop spanning the `handle` still evicts -- with the
located "this effect operation has no lowering here" diagnostic, which names the
hoist-into-a-helper workaround. Verified pre-existing (identical diagnostic on
the pre-fix compiler), so it is a designed eviction, not a regression.

The elaborator-diagnostic fallback (direction 2) is moot: the shape compiles and
runs correctly now.

## Workaround (no longer needed)

The natural form works now. Historically the state had to live in a heap cell
written through by hand:

    (defn cell-new []                      : ptr<void> ```c return calloc(1, sizeof(int64_t)); ```)
    (defn cell-get [c : ptr<void>]         : int       ```c return *(int64_t *)c; ```)
    (defn cell-put [c : ptr<void> v : int] : int       ```c *(int64_t *)c = v; return v; ```)

    (let [s (cell-new)]
      (handle (do (counter-step) (counter-step) (counter-step))
        (Get []  k) (resume k (cell-get s))
        (Put [v] k) (do (cell-put s v) (resume k nil)))
      (println (cell-get s)))            ; => 3

`stdlib/ref.tur` still ships `ref-new` / `ref-get` / `ref-free` with no
`ref-set`, so there is no pure-Turmeric mutable cell to reach for. That is now
a standalone stdlib gap rather than something this defect forces anyone into --
worth adding on its own merits, not tracked here.

The state section of `docs/guides/effects-vs-monads.md` used to teach the
heap-cell form and explain why; it now shows the `^mut` version (both the plain
and sweet-exp samples were run to confirm they print `3`), and the Sharp edges
list records the loop-carried limitation that survives.

## Where the truth is pinned

`tests/fixtures/effect-handler-clause-setbang-enclosing-mut/` runs the canonical
state handler plus the stale-read, typed-float, cstr, read-modify-write,
`while`-in-clause, and multishot-write shapes on BOTH paths (3, 7, 10.1,
touched, 13, 10, 211, 1). Suites green: `tests/run.sh` 2573/0,
`tests/run-turi.sh` 1760/0/705, with no snapshot churn -- the emitted C changes
only for programs whose clause actually touches an enclosing mutable.
