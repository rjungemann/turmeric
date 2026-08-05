# A `#lang` reader switch permanently breaks generic typeclass resolution

**Severity:** medium. Display-level in every case seen so far, but the defect is
in *constrained-instance resolution*, not in Show -- so the blast radius is
every generic typeclass dispatch in a long-lived REPL, not just rendering.

**Status:** open. Found 2026-07-29 while verifying the WASM half of
[web-repl-lang-switch-drops-stdlib](../archive/history/web-repl-lang-switch-drops-stdlib.md).
Reproduces natively; **no emscripten needed**.

## Repro

```
$ printf '(show (:: #map{:a 1} (Map Sym int)))\n#lang turmeric/neoteric\n(show (:: #map{:a 1} (Map Sym int)))\n' | tur repl

=> "#map{:a 1}"                       <- before the switch: correct
; reader set to turmeric/neoteric (session reset)
=> "#map{108920370806512 1}"          <- after: element shows the raw carrier
```

The element types are written out explicitly at both call sites. Nothing about
the expression changed; only a reader switch happened in between.

Two properties sharpen it:

- **Monomorphic instances are unaffected.** `(show :hello)` returns `":hello"`
  before and after the switch. Only instances resolved through a *constraint*
  (`Show [Map]`'s `^Show K ^Show V`) regress.
- **It does not recover.** Switching back to the original reader does not
  restore correct resolution:

  ```
  #map{:a 1}            => #map{:a 1}
  #lang turmeric/sweet  ; reader set to sweet-exp (session reset)
  #lang turmeric        ; reader set to turmeric (session reset)
  #map{:a 1}            => #map{108920370806512 1}
  ```

  This distinguishes it from the stdlib-drop bug in the sibling report, whose
  fix is symmetric on switch-back and verified so by
  `tests/turi/lang-switch-prelude.c`.

## Not the display path, and not the recent auto-show fix

Worth stating explicitly, because this surfaced next to a fix in that area and
looks superficially like the same bug:

- The repro uses an explicit `(show ...)` call whose result is a `String`. It
  never enters `turi_try_show_by_tag`, so the auto-show display tier is not
  involved.
- Verified directly: with the `map-show-keyword-key-raw-int` root-cause-B
  seeding compiled out (`if (0 && recv_ty)`), the before/after divergence above
  is byte-identical. The defect predates that fix and is independent of it.

## What is *not* broken

Instrumenting the auto-show path across the switch shows every inbound signal
intact -- so this is not type information being lost on the way in:

```
[showty] tag='Map' recv_ty=0x... kind=21 head='Map' nargs=2: [0]kind=58 [1]kind=3
[seed]   owner_inst=0x... n_constraints=2 binds: V=kind3 K=kind58
```

Both lines are **identical before and after** the switch: the elaborated result
type still carries concrete `(Map Sym int)`, the instance is found, it still
reports 2 type-param constraints, and `K`/`V` are bound to the right kinds
(58 = `TY_SYM`, 3 = int). The correct types reach the instance body and the
body still renders the element as an int.

So the failure is in **re-resolution inside the generic body** after a session
reset -- `map-show-loop`'s `(show (:: (hamt/iter-cur-key iter) K))` no longer
resolving `K` to `Show [Sym]` -- not in anything upstream of it.

## Root cause direction (unproven)

A reader switch calls `turi_env_reset_to_prelude`, which rewinds `src_acc` to
the pinned prelude and drops the `ElabSession`; the prelude is then
**re-elaborated** (not re-evaluated) when the session is rebuilt. That rebuild
produces a fresh `TypeClassEnv`, and `env->last_tc_env` is repointed at it.

The most likely candidates, in the order worth testing:

1. **Stale vs. fresh `TypeClassEnv` identity.** `gde_reresolve_method` matches
   against `tc_env->instances` and the interpreter compares some typeclass
   objects by pointer. If the instance reachable from the re-elaborated body
   and the one in the rebuilt registry are now different objects for the same
   logical instance, a pointer-identity check would silently miss.
2. **Duplicate instances after re-elaboration.** If the rebuild appends rather
   than replaces, the by-head-name scan may hit a first-match duplicate whose
   `method_impls` are the pre-reset ones.
3. **`type_param_constraints` surviving but their `tyvar` pointers not.**
   `frame_lookup_tyvar` compares `tb->name == name || strcmp(...)`, so it is
   robust to re-interning, but the *ascription* re-resolution path may compare
   tyvar identity more strictly.

Note that the constraint count survives the reset (`n_constraints=2` above), so
whatever breaks is finer-grained than the instance record being lost.

## Why this matters beyond rendering

Every symptom found so far is a raw carrier integer in output, which reads as
cosmetic. It is not: the same mechanism selects instances for any constrained
generic call, so after a `#lang` switch a REPL session can silently dispatch to
the int-carrier representative for *any* typeclass, not just `Show`. Worth
checking `Eq`/`Ord`/`Hash` over a `Map`/`Set` post-switch before assuming the
impact is display-only -- a wrong `Eq` instance is a wrong answer, not a
cosmetic one.

## Blast-radius probe: `Eq` is measurably unaffected (2026-07-29, Linux)

This report asks, correctly, whether the impact is display-only -- "worth
checking `Eq`/`Ord`/`Hash` over a `Map`/`Set` post-switch before assuming".
Probed; the answer so far is **yes, display-only**, but the probe needs one
piece of care to be worth anything.

**`Sym` elements cannot answer the question.** Sym equality *is* pointer
equality, so a regression to `Eq[int]` on the carrier returns the same answer as
`Eq[Sym]`. `(eq? #map{:a 1} #map{:a 1})` is `true` before and after the switch,
and that measures nothing -- the same coincidence that made `#map{7 70}` render
correctly. The discriminating element type is `cstr`: `Eq[cstr]` compares
content, `Eq[int]` compares the pointer.

Validated first, compiled, that the probe discriminates at all -- a runtime-built
string and a literal of the same content are at **different addresses**, and
`eq?` on them is `true`:

```
DIFFERENT-POINTER
eq?=true
```

Then, one REPL session, the same value, across the switch:

```
(load "stdlib/str-build.tur")
(eq?  (:: (vec-of (str-concat "a" "b")) (Vec cstr)) (:: (vec-of "ab") (Vec cstr)))  => true
(show (:: (vec-of (str-concat "a" "b")) (Vec cstr)))                                => "[ab]"
#lang turmeric/neoteric                       ; reader set (session reset)
(load "stdlib/str-build.tur")                 ; re-loaded: the rewind drops prompt-loaded code
(eq?  (:: (vec-of (str-concat "a" "b")) (Vec cstr)) (:: (vec-of "ab") (Vec cstr)))  => true
(show (:: (vec-of (str-concat "a" "b")) (Vec cstr)))                                => "[88098369185552]"
```

`Eq [Vec]` still returns content equality; `Show [Vec]` over the *identical*
receiver regresses to the raw carrier. Both are constrained instances
(`[(Eq A)]` / `[(Show A)]`) whose bodies are structurally the same shape --
re-ascribe the element to the tyvar (`(:: (vec-get v i) A)`) and call the class
method through a `^Eq A` / `^Show A` helper defn (`vec-eq-loop`,
`vec-show-loop`).

So this report's "every generic typeclass dispatch" framing is **wider than what
is measured**: at least one other constrained instance over the same receiver
type, with the same body shape, is unaffected. That is also a debugging lead --
a working and a broken case that differ in almost nothing.

**Not established: why.** Two candidates, untested:

1. The inner `eq?` on a `cstr` receiver resolves through the *runtime-tag*
   route, which works because `cstr` has its own carrier -- in which case `Eq`
   never exercises the broken tyvar re-resolution and the blast-radius question
   is still open for any element type riding the int64 carrier.
2. `Eq`'s tyvar re-resolution genuinely survives the reset and `Show`'s does not.

Candidate 1 would mean the narrowing above is weaker than it looks -- an element
type with an int64 carrier (`Sym`, an opaque, a nested `Vec`) is the case to
probe next, with a class whose answer is not pointer-equality by coincidence.
`Ord`/`Hash` over a collection were not probed at all.

## Fix directions

- Pin down which of the three candidates above it is, by dumping the resolved
  `TypeClassInstance*` for the element `show` inside `map-show-loop` before and
  after a switch and comparing against the registry.
- If it is instance identity (1 or 2), the reset should either preserve the
  existing `TypeClassEnv` across a re-elaboration that adds no new instances, or
  re-point every already-elaborated body at the rebuilt registry -- not leave the
  two halves disagreeing.

## Coverage to add with the fix

`tests/turi/lang-switch-prelude.c` is the natural home: it already drives
switch / switch-back / third-reader and asserts the stdlib is reachable. It
should additionally assert that a *generic* instance still resolves after each
switch (e.g. `turi_try_show_by_tag` on `#map{:a 1}` returning `#map{:a 1}`),
which is exactly the assertion that would have caught this. Its current checks
are all `map-count`-style integer results, which is why the defect survived
that fix.
