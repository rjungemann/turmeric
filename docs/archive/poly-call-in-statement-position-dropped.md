# A discarded call to a parametric function is dropped, side effects and all, unless it instantiates at `int`

**Severity: high.** Silent miscompile with no diagnostic. The call does not
happen -- its effects are lost, not deferred -- and the program runs on to print
a plausible wrong answer. Only `int` instantiations survive, which is why it has
gone unnoticed: `int` is the carrier and the overwhelmingly common case.

**Status:** RESOLVED 2026-08-26. Two cases in `emit_stmt`'s
"no side effects -- emit nothing" list wrapped an operand and returned without
emitting it, so the wrapped call was deleted along with its effects:

- **`EX_REINTERPRET`**, the carrier bridge placed around a polymorphic call at a
  non-carrier type. This is why `int` was the exception -- it needs no bridge,
  so nothing wrapped it.
- **`EX_ASCRIBE`**, whose own comment already said "type erased, delegate to
  inner" while the code returned without delegating. Found by testing the
  neighbours rather than assuming the first fix was the whole bug.

Both now emit their operand as a statement: the conversion is discarded, which
is what statement position means, and the effect survives. **Zero snapshot drift
across all 147 `expected.c` fixtures**, and the suite went from 2697/1 to
**2698/0** -- the regression fixture had been red on purpose and flipped green
on the fix.

## The sweep: it was eight cases, not two

The first fix took the two with repros. A follow-up sweep asked the structural
question instead -- *which kinds in that list hold a sub-expression?* -- and
found six more, every one of them in the list on the strength of the WRAPPER
being pure:

| kind | was described as | holds |
|---|---|---|
| `EX_CAST` | "pure expression" | `cast_.expr` |
| `EX_UNION_INJECT` | "pure struct literal" | `union_inject_.value` |
| `EX_ANY_TYPE_OF` | "pure read" | `any_type_of_.value` |
| `EX_ANY_CAST` | "pure unbox" | `any_cast_.value` |
| `EX_ANY_IS` | "pure tag test" | `any_is_.value` |
| `EX_POLY_WRAP` | "pure struct literal" | `poly_wrap_.inner` |
| `EX_EXISTS_PACK` | "pure boxing" | `exists_pack_.value` |
| `EX_CONS_LIST` | "allocation side-effects handled in emit_value" | `cons_list_.items[]` |

That last comment is worth reading twice: it names the function that would have
handled the effects, and `emit_value` was never called.

**Three are demonstrably reachable**, and were silently dropping calls before
the sweep -- proven by reverting it and watching the effects vanish:

```turmeric
(cast (effAny) cstr)      ;; EX_ANY_CAST     -- dropped
(type-of (effAny))        ;; EX_ANY_TYPE_OF  -- dropped
(:: (eff) :any)           ;; the widening wrap -- dropped
```

All three are in `tests/fixtures/poly-statement-position-effect` now. The other
five have no reachable statement-position spelling found so far, so they are
fixed as defence rather than against a repro -- stated plainly rather than
implied, since "fixed" reads stronger than the evidence for those five.

**`EX_CPS_CONT_APP` was deliberately left alone.** It also holds sub-expressions,
but applying a continuation is the effect rather than a wrapper around one, so
the rule used here does not apply to it and it needs its own answer.

Zero snapshot drift again across all 147 fixtures, and the suite stays 2698/0.

## Repro

```turmeric
(defn polyA [A] [x : A] : A (do (println "  ran") x))

(defn main [] : int
  (println "poly@bool:")  (polyA true)
  (println "poly@int:")   (polyA 1)
  (println "poly@float:") (polyA 7.1)
  (println "poly@cstr:")  (polyA "s")
  0)
```

```
poly@bool:
poly@int:
  ran
poly@float:
poly@cstr:
```

**One of four runs.** The other three calls are not emitted at all.

## Scope

The three conditions are all required:

| | discarded | result used |
|---|---|---|
| **parametric**, `A = int` | runs | runs |
| **parametric**, `A = bool` / `float` / `cstr` | **DROPPED** | runs |
| monomorphic `: bool` | runs | runs |
| monomorphic `: int` | runs | runs |

So it is not "bool is mishandled" and not "discarded calls are dropped" -- it is
*parametric*, instantiated at anything that is not the `int` carrier, in
statement position.

Confirmed in the emitted C: for the dropped cases the enclosing function simply
has no call. In the `with-untrailed` case that found this, the specialization
was declared and defined, and `main` never referenced it.

## Why this is worth a high severity

Statement position is where side effects live. A polymorphic helper called for
its effect -- a setter, a logger, a `with-*` bracket, anything returning `bool`
for success -- silently does nothing, and the surrounding code keeps running.
There is no warning, no link error (the specialization is emitted, just never
called), and no crash.

It is also invisible to a test suite that checks printed output *of the thing
that was dropped*: the fixture that found this printed the same numbers whether
or not the body ran, because the body's effect was the only difference.

## Impact on SX2

`with-untrailed` (`stdlib/trail.tur`) is parametric and its natural use
discards a `bool`:

```turmeric
(with-untrailed (fn [] (bt-set! c 7)))   ;; bt-set! returns bool -> DROPPED
```

The write silently does not happen and trailing is never paused. The combinator
is correct; the call site is miscompiled. Landing it while this is open means
shipping a primitive whose documented example does not work, so the SX2 note
records the hazard.

`bt-scope` is unaffected in practice only because its bodies tend to return a
value the caller uses.

## Fix directions

1. **Find where a discarded call decides not to emit.** The asymmetry with `int`
   points at the carrier: the statement-position path probably emits only when
   the result type maps to the carrier ABI, and drops the rest instead of
   emitting-and-ignoring.
2. **A discarded call must always be emitted.** Dropping a call is only sound
   when the callee is known pure, and nothing here is checking that -- `polyA`
   above prints.
3. **Worth a diagnostic either way:** if some discard really is intentional, it
   should say so rather than silently deleting user code.

Regression fixture: `tests/fixtures/poly-statement-position-effect`. It is
**red on purpose** -- it asserts the correct output, so it goes green exactly
when this is fixed.

## Workarounds -- REMOVED 2026-08-26

Both were reverted in the same change that fixed the bug, per the note below.
`with-untrailed`'s docstring carries the natural example again, and
`sx2-trail-combinators` discards the result on purpose -- exercising the
spelling that used to be miscompiled is now part of what that fixture is for.

The original note follows, for the record.

## Workarounds to replace once this lands

Working around it means writing something other than the natural spelling, so
every site is a place the code says one thing and means another. Each must be
reverted, not left in place:

1. **`stdlib/trail.tur`'s `with-untrailed` docstring** carries a HAZARD block
   telling callers to bind the result rather than discard it, and its worked
   example is written in the bound-result form for that reason alone. Delete the
   block and restore the natural example:

   ```turmeric
   (with-untrailed (fn [] (bt-set! c 7)))     ;; what it should say
   ```

2. **`tests/fixtures/sx2-trail-combinators`** binds `with-untrailed`'s result to
   `_` with a comment naming this report. Once fixed, discard the result there
   instead -- that fixture should exercise the spelling users will actually
   write.

3. **Any later `with-*` bracket in the trail or search work** will hit the same
   wall. If more appear before this is fixed, add them here rather than
   scattering the same `let [_ ...]` idiom with no pointer back.

**How to prove the workarounds are no longer needed:** revert site 1's example
to the discarded form and run it. If the write happens, the bug is gone. Do not
infer it from `poly-statement-position-effect` alone -- that fixture pins the
general rule, while these sites pin that the rule reaches real stdlib code.

Tracked alongside the others in
[workarounds-to-remove](workarounds-to-remove.md).
