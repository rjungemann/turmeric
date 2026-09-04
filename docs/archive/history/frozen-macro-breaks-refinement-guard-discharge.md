# A macro-wrapped region (`frozen`) breaks refinement guard-discharge; the inline `let`-borrow works

**RESOLVED 2026-07-26 (fix direction 1).** Added `rt_form_ident` in
`src/compiler/elab_fns.c`: crossing identity now falls back from pointer
equality to source `Span` + head symbol + arity, used in both
`rt_form_occurrences` and `rt_collect_path_conds`. A macro-copied crossing
preserves its source span, so the guard on its path is recovered; the `!= 1`
ambiguity decline is untouched, so a macro that DUPLICATES a crossing at one
span still bails. The `ecs/freeze` `frozen` macro now discharges a
`#reads`-guarded read (`1 proven`). Validated: macro proves, inline still
proves, a macro region with NO guard and a macro region with a `set!` between
guard and read both still reject (W0372), a crossing-duplicating macro where
both reads are genuinely guarded proves both (correct), the four
`errors/refine-stateful-*` negatives still reject, the source fuzzer reports 0
soundness bugs, and the full suite is 2367/0. Regression guard:
`tests/fixtures/refine-stateful-frozen-macro`. The second dependency below (the
`GameWorld` facade with a `^unique ^mut` despawn) is an RE1 design step, tracked
in the ecs-refinement plan -- not a compiler bug. Original report follows.

**Severity:** medium (blocks the *ergonomic* `#reads` surface -- the shipped
`ecs/freeze` `frozen` macro -- from composing with refinement congruence; the
inline `(let [_ (& w)] ...)` form is a sound, working substitute, so it is a
usability wall for RE1, not a soundness hole). Not a miscompile: the failure mode
is a spuriously *unproven* crossing (`TUR-W0372`), never a wrongly-proven one.

## Summary

A `#reads`-guarded read discharges when the region is written inline:

```turmeric
(let [__frz (& w)]                         ;; inline immutable borrow
  (if (alive? w e) (get-Pos! w e) -1))     ;; => refine: 1 proven
```

but the *same* read fails to discharge when the region is the `frozen` macro
(`ecs/freeze`), or any macro that wraps the body:

```turmeric
(frozen w                                  ;; macro: `(let [__frozen-borrow (& ~w)] ~@body)`
  (if (alive? w e) (get-Pos! w e) -1))     ;; => refine: 0 proven, 1 unknown  (TUR-W0372)
```

Reproduces with a one-line *local* macro too (so it is not `ecs/freeze`- or
cross-module-specific):

```turmeric
(defmacro lfrozen [w & body] `(let [__lb (& ~w)] ~@body))
```

## Root cause (traced to file:line)

The guard `(alive? w e)` is recovered for a crossing by
`rt_push_cs_path_conds` (`src/compiler/elab_fns.c:1699`), which walks the
caller's body (`cs->caller_body`) looking for the crossing form (`cs->call_form`)
and appends the `if`-conditions on the path. It first guards against ambiguity:

```c
if (rt_form_occurrences(cs->caller_body, cs->call_form, 0) != 1) return saved;  // elab_fns.c:1705
```

`rt_form_occurrences` (`elab_fns.c:1526`) matches by **pointer identity**
(`node == target`). Instrumented, the count is:

- inline: **1** -> guards collected -> `(alive? w e)` becomes a hypothesis ->
  the goal `alive?(w,e)` (congruent via `#reads` + frozen) discharges.
- macro:  **0** -> declines -> no guard hypothesis -> the goal has nothing to
  discharge against -> unknown.

The count is 0 because **macro expansion copies the body** (quasiquote
reconstructs `~@body` into fresh `Form` objects), so the crossing that gets
elaborated and registered as `cs->call_form` is a *different pointer* than the
crossing node reachable in `cs->caller_body` (the source defn body). The comment
at `rt_push_cs_path_conds` already anticipates the sibling case ("a macro that
shares a node can produce" >1 route); this is the mirror -- a macro that *copies*
the node produces 0.

Confirmed by instrumenting the encoder side too: in the macro case the GOAL
crossing IS recognized as frozen/congruent (`enc_reads_arg_frozen` returns true),
but the GUARD hypothesis is never encoded with the frozen set -- because it was
never collected as a hypothesis in the first place.

## Fix directions

1. **Span-based crossing match (preferred).** When pointer identity yields 0,
   fall back to matching `call_form` against `caller_body` nodes by source
   `Span` (a macro-copied form retains its source span) plus head symbol +
   arity, in BOTH `rt_form_occurrences` and `rt_collect_path_conds`. Keep the
   `!= 1` decline so a genuinely ambiguous (>1) match still bails -- the change
   only rescues the "copied once" case, never relaxes the ambiguity guard.
2. **Post-expansion `caller_body`.** Capture `cs->caller_body`
   (`refine_fill_call_site_env`, `elab_fns.c:1495`) from the fully
   macro-expanded body so its nodes are the same objects the crossings were
   elaborated from. Larger change; touches how the defn body form is threaded.

Either must be validated hard: the macro case proves, the inline case still
proves, `errors/refine-stateful-{mutation-invalidates,aliased-mutation,shadow-despawn,no-region}`
still reject, and the full suite stays green (this path is soundness-sensitive --
a wrong guard attribution would be a real miscompile).

## Impact on RE1 (ecs refinement)

RE1's accessor guards are meant to read `(frozen w (if (alive? w e) (get-Pos! w e) ...))`.
Until this is fixed, RE1 must spell the region inline as
`(let [_ (& w)] ...)` (sound and working) or the guard will not discharge. See
`docs/upcoming/v1/ecs-refinement-typed-apis-plan.md` (RE1) and
`docs/archive/refine-stateful-measures-plan.md`.

## Second RE1 dependency (noted here, separate issue)

RE1 also needs the world's *mutator* to take `^unique ^mut` over an owned world
so a `frozen` borrow locks it out. Today both `ecs/world`'s `world-despawn!`
(`gens : int`) and `ecs/sized-world`'s `sized-despawn` (`s : WorldState`, a
by-value COPY handle) mutate through a value/`:int` handle, which a borrow cannot
lock out -- and a despawn *call* between guard and read is not a `set!`/region-exit,
so the encoder would not invalidate it. RE1 therefore needs a `GameWorld` facade
that owns the state and gates despawn `^unique ^mut` (matching the RE1 spec's
`GameWorld` and the RM-S0 dogfood), not a direct refinement over the current
by-value world API.
