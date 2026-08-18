# `#reads` cannot name more than one parameter

**Status:** RESOLVED 2026-08-18.
**Severity:** medium (a real expressiveness hole in the refinement surface --
a measure that reads two `^borrow` parameters has no way to say so; the
adjacent silent-drop bug is fixed, this is not)
**Found:** 2026-08-18, chasing "multiple `#reads` params" -- for which no
report existed.

## Summary

`#reads` names the one `^borrow` parameter whose mutable state a measure
reads. There is no spelling for two, and the representation cannot hold two:

```c
/* expr.h */
uint32_t reads_param_plus1;   /* 1-based param index; 0 == none */
```

Its sibling `#writes` takes either form -- `#writes w` or
`#writes [w g]` -- and the reader comment there explains exactly why the
bracketed spelling was needed (the return marker `:` reads as a symbol, so a
greedy scan cannot find the end of the frame). `#reads` never grew the
equivalent.

So a measure over two pieces of borrowed mutable state is simply not
expressible:

```turmeric
;; wanted, no spelling for it
(defn linked? [^borrow w : World ^borrow g : Grid e : int] #reads [w g] : bool
  ...)
```

## What each spelling does today

Measured against the fix in the same commit as this report:

| spelling | before | now |
|---|---|---|
| `#reads w` | accepted | accepted (unchanged) |
| `#reads [w]`, `#reads [w g]` | `unexpected character '['` | TUR-E0024, naming `#reads` |
| `#reads w #reads g` | **accepted, second silently overwrote the first** | TUR-E0024 |
| `#reads w #reads g #reads t` | `unknown function or operator 'reads'` | TUR-E0024 |
| `#reads w g` | `type annotation ':' is only valid after a parameter name` | unchanged |
| `#writes [w g]` | accepted | accepted |
| duplicate `#writes` | TUR-E0381 | unchanged |

The silent-drop row was the dangerous one. The signature walk runs two
annotation slots so `#reads` and `#writes` can appear in either order; two
`#reads` fit that loop, and the second just reassigned
`reads_param_plus1_defn`. `#reads` is described in
`src/runtime/experiments.c` as "the one TRUSTED claim the refinement solver
believes", so silently keeping the wrong one handed the solver a claim the
author never wrote. That half is fixed (TUR-E0024, two `errors/` fixtures).

**The expressiveness hole is not fixed.** An author who needs two read
parameters now gets a clear error instead of a silent miscompile, which is
strictly better but still a dead end.

## Why this was not just widened

`reads_param_plus1` has 13 references across `elab_fns.c`, `expr.h`,
`refine_collect.c`, `refine_collect.h`. Widening it to a set is mechanical in
most of them, but one is a semantic decision rather than a refactor:

```c
/* refine_collect.c */
if (!pure && info.reads_param_plus1 != 0 &&
    enc_reads_arg_frozen(E, f, info.reads_param_plus1))
```

This is the congruence override -- the measure is treated as congruent
because its single read parameter is provably `frozen` at the call site. With
a set, the rule has to become "**every** named read parameter is frozen"
(conjunctive) rather than "any", and that is the difference between a sound
override and an unsound one. Getting it backwards would silently re-admit the
crossing checks `#reads` + `frozen` exists to elide.

That is an owner call on trusted-claim semantics, not a drive-by, so it wants
a short plan rather than an opportunistic patch.

## Fix directions

1. **Widen the representation.** `reads_param_plus1` -> a small fixed-width
   set (a bitmask over param indices is enough; parameter counts are small).
   Update the congruence override to require ALL named params frozen.
2. **Add the vector reader form.** `#reads [w g]`, mirroring `#writes`
   verbatim -- the reader already has the bracketed-frame code to copy from,
   and `read_writes_annot` sits directly below `read_reads_annot`.
3. **Keep the single-symbol form** working; `#writes` supports both and this
   should too.

Worth checking while doing (1): `docs/upcoming/mutable-globals-plan.md:300`
already writes `#reads [*cache*]` -- a bracketed frame over *globals* rather
than parameters. If that plan lands, the vector form is needed anyway, and
the two should agree on one syntax rather than arriving separately.

## Workarounds until then

- Split the measure so each one reads a single parameter.
- Pass one receiver that owns both pieces of state, and read it as one
  parameter.

Both are honest but lossy: the first cannot express a predicate that is
genuinely joint over two states, and the second forces a data-model change to
satisfy an annotation limit.

## RESOLVED (2026-08-18)

Implemented, following the three fix directions above.

**Representation.** `uint32_t reads_param_plus1` (a single 1-based index) ->
`uint64_t reads_params_mask` (bit i == param i), on both `Binding` and
`RefineFnInfo`. Parameters past bit 63 are rejected with TUR-E0024; an arity
that high already trips the TUR-W0041 lint.

**Reader.** `read_reads_annot` now mirrors `read_writes_annot`: `#reads w`
or `#reads [w g ...]`. The single-symbol form is unchanged.

**The semantic decision -- conjunctive.** `enc_reads_arg_frozen` became
`enc_reads_args_frozen`, requiring **every** named parameter to be frozen at
the call site. This is the arm the report flagged as the one real decision,
and it is verified in all four quadrants rather than asserted:

| frozen | result |
|---|---|
| `w` and `g` | proves (prints 42) |
| `w` only | TUR-W0372, grant withheld |
| `g` only | TUR-W0372, grant withheld |
| neither | TUR-W0372, grant withheld |

Under the unsound "any named param frozen" reading, rows 2 and 3 would have
compiled -- silently eliding a crossing check that is not justified. Both the
positive and the partial-frozen negative are pinned as fixtures
(`refine-reads-multi-param-frozen`,
`errors/refine-reads-multi-param-partial-frozen`), and the negative's header
says why it exists, so a future change that relaxes the quantifier fails
loudly.

**Empty frames stay rejected.** `#writes []` usefully asserts "this body
writes nothing"; an empty read frame says exactly what omitting the
annotation says, and one claim with two spellings would give the encoder two
ways to ask the same question. `#reads []` is TUR-E0024.

**Diagnostics kept** from the interim commit, retargeted: duplicate frame,
repeated name, unknown name, empty frame -- all TUR-E0024, all with fixtures.
The `tur explain TUR-E0024` text and
`docs/guides/stateful-refinements-guide.md` now document the vector form and
the conjunctive rule.

**Also fixed as a consequence:** TUR-W0383's message rendered only
`params[reads_param_plus1 - 1]`, so on a multi-parameter frame it would have
quoted just the first name and misreported which claim is broken. It now
renders the whole frame.

Suite: **2626 passed, 0 failed.**

Left for whoever lands `docs/upcoming/mutable-globals-plan.md`: that plan
writes `#reads [*cache*]` -- a bracketed frame over *globals* rather than
parameters. The bracket syntax now exists and is free to reuse; what still
needs deciding is whether globals share this mask or get a parallel one.

### Follow-up: the open question was researched and it is NOT safe

The note above left "whether globals share this mask or get a parallel one"
open. Researching it turned up a soundness bug in the **existing**
single-parameter grant, filed as
[`reads-grant-survives-callee-global-write`](../reported/reads-grant-survives-callee-global-write.md):
a frozen GLOBAL can be written by a callee with no trace at the call site, so
the congruence hypothesis is never invalidated and a refinement precondition
that is false at the crossing gets statically proven.

That settles the design question. A global entry must not grant congruence on
the strength of a frozen region alone, so its semantics differ from a
parameter entry's -- which is a further argument for the parallel-field
representation rather than a shared mask.
