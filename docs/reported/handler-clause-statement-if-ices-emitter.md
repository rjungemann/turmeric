---
status: open
severity: high
discovered: 2026-07-29
area: compiler (CPS coloring / emitter, src/compiler/emit_expr.c)
---

# An `if` in statement position inside a `handle` clause ICEs the emitter

## Summary

A handler clause whose body contains a conditional in **non-tail (statement)
position** aborts the compiler:

    tur: internal error: effect form (EX kind 57) reached the direct/fiber
    emitter (fiber effect runtime deleted)

Kind 57 is `EX_PERFORM`. The clause shape evicts the enclosing function from the
CPS backend, so its `perform` falls through to the direct emitter, which no
longer has a lowering for it and aborts by design
(`src/compiler/emit_expr.c:7929-7938`). The comment there asserts this path is
"corpus-verified unreachable" -- it is reachable.

This is not an exotic shape. `while` and `when` both desugar into it, so any
handler clause that loops or does conditional work before producing its value
hits it. Found while reworking `docs/guides/effects-vs-monads.md`: the natural
way to write a state handler or a "resume once per choice" multi-shot handler
runs straight into it.

## Repro

Minimal -- the `if` is discarded by the `do`, so it is in statement position:

    $ cat > /tmp/h.tur <<'EOF'
    (defeffect Ask [] : int)
    (defn main [] : int
      (println (handle (perform (Ask)) (Ask [] k) (do (if (> 1 0) 1 2) 99)))
      0)
    EOF
    $ ./build/tur run /tmp/h.tur
    tur: internal error: effect form (EX kind 57) reached the direct/fiber emitter (fiber effect runtime deleted)

Moving the same `if` into tail position compiles and runs:

    (handle (perform (Ask)) (Ask [] k) (if (> 1 0) 1 2))   ; => 1

It reproduces with the `perform` in a callee as well as inline in the `handle`
body, and with or without a `resume` in the clause.

### Shapes that reach it through desugaring

    ;; ICE -- clause never resumes, `while` in the clause
    (Ask [] k) (let [^mut i 0] (while (< i 3) (set! i (+ i 1))) i)

    ;; ICE -- `resume` inside the loop
    (Choose [lo hi] ^multishot k)
      (let [^mut a 0 ^mut i lo]
        (while (<= i hi) (set! a (+ a (resume k i))) (set! i (+ i 1)))
        a)

    ;; ICE -- `when`, no loop at all
    (Ask [] k) (do (let [^mut i 0] (when (< i 3) (set! i 9))) 99)

The second one is the blocking case in practice: it is how you would fold a
multi-shot continuation over a range, and there is no way to write it.

## Root cause

Not pinpointed beyond the abort site. The `perform` is reaching
`emit_expr.c:7931` (`case EX_PERFORM:` in the direct/fiber emitter) instead of
`emit_cps_ir`, which means the CPS coloring analysis decided the enclosing
function does not need converting, or it was converted and then evicted. The
statement-position conditional in the clause is what flips that decision --
`--dump-cps-coloring` on the minimal repro against the tail-position variant
should show which.

Note the adjacent precedent at `src/compiler/emit_stmt.c:439-448`: a discarded
`reset` in statement position was previously lowered to `__builtin_trap()` for
the same underlying reason (a colored function evicted from the CPS backend),
and was fixed by routing statement position through `emit_value`. This looks
like the same class of gap on the `perform` side.

## Fix directions

1. Find the coloring decision first. If a statement-position `if` inside a
   handler clause is simply not being walked when computing whether the
   enclosing function can reach a control operator, that is the bug and the fix
   is in the coloring walk, not the emitter.
2. If the function is genuinely evicted for an unrelated reason, the eviction
   needs to be blocked when the function contains a `perform` -- an evicted
   function with a `perform` in it has no valid lowering at all.
3. Either way, add fixtures for the three shapes above. A multi-shot handler
   that resumes in a loop is the one that matters most; it is the only way to
   express bounded nondeterminism directly, and its absence is why
   `effects-vs-monads.md` documents the payload-indirection workaround instead.

## Workarounds

- Keep conditionals in tail position inside handler clauses.
- Hoist the conditional work into a helper function called from the clause.
- For multi-shot fan-out, pass the resumption strategy through the effect
  payload so the loop lives in an ordinary function; see the nondeterminism
  section of `docs/guides/effects-vs-monads.md`.
