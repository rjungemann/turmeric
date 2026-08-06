---
status: RESOLVED 2026-08-05 (all three shapes; two landings same day)
severity: was high
discovered: 2026-07-29
area: compiler (CPS admission, src/compiler/emit_cps_ir.c + src/passes/cps_ir.c)
---

# An `if` in statement position inside a `handle` clause ICEs the emitter

> **RESOLVED 2026-08-05, in two landings.** The first fixed the `if` and
> `when` shapes and replaced the ICE with a located error; the second admitted
> `CT_LOOP` in a handler case, which fixes the `while` shape -- including the
> multi-shot fold the report called "the blocking case in practice". Three
> separate root causes were found where the report assumed one -- see
> [Status](#status-2026-08-05) and [Resolution of the third
> shape](#resolution-of-the-third-shape-2026-08-05-second-landing) at the
> bottom. The "Root cause" section below is superseded: it guessed at the
> coloring analysis, and the coloring analysis was never wrong.
>
> One narrow eviction remains **by design**, with a located diagnostic: a
> `perform` of an outer-handled effect from inside a loop inside a clause
> (`tests/fixtures/errors/effect-handler-clause-loop-perform-unsupported`).
> And one **new divergence was found on the way**: turi aborts on a multishot
> resume from the second iteration of a `while` -- spun off into
> [turi-multishot-resume-in-while-aborts](../reported/turi-multishot-resume-in-while-aborts.md),
> which is why the fold fixture is `requires.compiled`.

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

## Status 2026-08-05

The three "shapes that reach it through desugaring" turned out to be **three
different blockers**, not one. The report's framing -- and its fix direction 1,
"find the coloring decision first" -- pointed at the coloring analysis, which is
not involved: `--dump-cps-coloring` reports `main: colored` for every shape here,
including the ones that ICE. The function is colored and then **evicted during
CPS-IR admission** (`TUR_TRACE_EVICT` says `BODY-STRUCT-OR-TAINT eff=1 main`),
which is fix direction 2's territory.

| Shape | Root cause | State |
| --- | --- | --- |
| `(do (if c a b) v)` -- the title shape | `handle_case_ok` had no `CT_LETCONT` arm | **fixed** (first landing) |
| `(do (when c a) v)` | `cps_tail(b, NULL, kont)` on the absent else -> `CT_UNSUPPORTED("null")` | **fixed** (first landing) |
| `(while ...)` in a clause | `handle_case_ok` has no `CT_LOOP` arm | **fixed** (second landing, below) |

### What was fixed

**The join point.** A conditional in statement position does not lower to a tail
`CT_IF`; it lowers to `letcont j(x) = <rest> in if c then (j a) else (j b)` --
a join point. `handle_case_ok` admitted `CT_IF` but had no `CT_LETCONT` arm and
no `KK_VAR` arm on `CT_APPCONT`, so the whole case fell to `default: return
false`. Both arms are now admitted, and `handle_case_ok` first runs
`joins_closed_rec` with a fresh def set: `emit_lifted` gives each case its own
frame with a fresh join stack, so a jump that escapes to an enclosing join has
no slot and must still be rejected. That check is what makes admitting `KK_VAR`
safe rather than hopeful -- and it is the same check `joins_closed_rec` already
applied to a case body one level down, inside its own `CT_HANDLE` arm.

**The one-armed `if`.** `(when c body)` is an `EX_IF` with a NULL else. Both
`EX_IF` sites in `cps_ir.c` handled that by calling `cps_tail(b, NULL, kont)`,
which lands in the null guard and produces `CT_UNSUPPORTED`. A one-armed `if` is
nil-typed -- the checker rejects it otherwise -- so the missing arm now delivers
unit to the same continuation the taken arm does (`cps_tail_unit`). The null
guard in `cps_tail` is left alone: it is still the right answer for a genuinely
missing form.

`tests/fixtures/effect-handler-clause-statement-position` covers both, asserting
**values** rather than just compilation -- the discarded branch's side effects
must still happen and the untaken branch's must not. Compiled and `--interpret`
output agree.

### What is still open

*(Superseded by the second landing -- kept for the reasoning.)* A `while` in a
handler clause. `cps-while-native` lowers a loop with an interior
control op into a synthesized tail-recursive `__cps` helper (`CT_LOOP`), and
`handle_case_ok` has no arm for one inside a lifted case. Admitting it is not a
missing switch arm like the two above -- it means emitting that helper from
inside a DK frame -- so it is left for whoever takes on the loop lowering. The
report's second listed shape (folding a multi-shot continuation over a range) is
this one, and remains the blocking case in practice.

## Resolution of the third shape (2026-08-05, second landing)

"Emitting that helper from inside a DK frame" turned out to hinge on one
question: **where does the loop's exit value go?** The helper's exit arm
delivers to its own `KK_RET`, and what that means depends on how the enclosing
frame delivers to the loop's `result_kont`. In an ordinary function or a
reset/handle continuation the target is a real DK chain -- thread it and
`dk_run` at the exit, the historical behavior. In a handler-case frame a
`KK_PROMPT` delivery is a plain `return` (the case helper's value IS its return
value, routed by `dk_perform`). So the loop helper gets a **return-direct
mode**: it returns its exit value, is entered with a `NULL` kont it never
reads, and the case helper returns the loop helper's return -- composing with
`dk_perform`'s routing exactly as a straight-line case value does
(`emit_loop`'s `ret_direct`).

Three pieces around that core:

- **Admission** (`handle_case_ok_rec` gains a `CT_LOOP` arm): params/inits at
  the slot gate; the body via a dedicated `case_loop_body_ok` grammar that is
  deliberately NARROWER than `term_core_ok` -- no interior `CT_PERFORM` /
  `CT_HANDLE` / `CT_RESET` / `CT_AWAIT`, because the helper's kont is NULL in
  this context and any op that threads it into the DK machine would hand it a
  null chain at run time. Those shapes still evict, to the located error the
  first landing added. `CT_RESUME` IS admitted: it targets the case's own `k`,
  not the chain.

- **The multi-shot fold works because `dk_invoke` already re-invokes.** The
  straight-line double-resume (`(+ (resume k lo) (resume k hi))`) was already
  supported; each in-loop `resume` is the same `dk_invoke` against the same
  `k`, so the loop only changes how many times. Verified by value:
  `(handle (+ 10 (perform (Choose 1 3))) ... fold ...)` prints `36` --
  `(10+1)+(10+2)+(10+3)` -- so the continuation demonstrably runs once per
  iteration.

- **`k` now binds as its int64 word in every case** (`emit_lifted`). The plain
  arm used to bind `DK *k` while the binding is int-typed; the loop-invariant
  threading then spelled the helper param `int64_t` from the binding's type and
  passed the `DK *` local -- an int-from-pointer hard error under GCC >= 14.
  Every read site already casts (`dk_invoke((DK *)(k), ...)`,
  `((DK *)(k))->consumed`, the `cont?` check), and the re-opening arm had
  already made this exact move for the same reason (env field stores), so the
  pointer spelling had no remaining consumer.

Fixtures: `tests/fixtures/effect-multishot-resume-in-loop` (the fold, by
value, including through a non-`main` function called twice) and the extended
`effect-handler-clause-statement-position` (plain `while` in a clause, which
absorbed the program from the deleted
`errors/effect-handler-clause-loop-unsupported` fixture exactly as that
fixture's comment instructed). The new
`errors/effect-handler-clause-loop-perform-unsupported` pins the one remaining
eviction's diagnostic and carries the same delete-me-if-admitted instruction.

The N6.5-gate note below stands: the `experimental_surface` exemption is still
wider than its rationale, and narrowing it is still its own change.

### The ICE is gone regardless

`emit_expr.c`'s `EX_PERFORM` / `EX_WITH_HANDLER` arm used to `abort()` on the
strength of a comment claiming the path was "corpus-verified unreachable". It is
reachable, by construction: any clause body outside the admissible subset evicts
its function and sends the clause's `perform` here. It now emits a located
`DIAG_ERROR` naming the likely cause and the workaround; `emit_program` already
fails the build on `diag_had_error()`, so this fails cleanly instead of
aborting. `tests/fixtures/errors/effect-handler-clause-loop-unsupported` pins
it, and says in its own comment that it should be *deleted* rather than fixed if
`CT_LOOP` is ever admitted.

Worth noting for whoever picks this up: the intended hard-error path for exactly
this situation **already exists** in `emit_cps_ir.c` (the N6.5 gate, "colored
function ... fell back to the direct emitter for a non-signature reason"), but
it is suppressed by `bool experimental_surface = g_opt_cps_tramp_resume`, which
defaults on -- so it never fires in a shipping build. That exemption was meant
for specific experimental shapes (a recursive `await`) and is written far wider
than its rationale. Narrowing it to the cases it names would have caught this
class at the gate, with the enclosing function's span, rather than downstream at
the `perform`. Not changed here: it is a routing decision with its own blast
radius, and the downstream diagnostic already removes the ICE.

### Note on the interpreter

Every shape here runs correctly under `tur --interpret` **except one found
during the second landing**: a multishot resume from the second iteration of a
`while` aborts turi (the compiled path runs it correctly, so the divergence now
points the other way). That is its own report --
[turi-multishot-resume-in-while-aborts](../reported/turi-multishot-resume-in-while-aborts.md).
turi evaluates handlers directly and never reaches the CPS admission gate, so
the error fixtures here carry `requires.compiled`.
