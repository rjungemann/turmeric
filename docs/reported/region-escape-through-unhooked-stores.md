# A region node stored by an unhooked primitive still dangles after the rewind

**Severity: low (was medium).** Filed 2026-09-06 during region-lock-hardening.
**Narrowed 2026-09-06**, same day: the class this was mostly about -- a node
handed to a hand-written inline-C body -- is fixed, and it was a **silent wrong
answer**, not the "documented contract" this report first called it. What is
left is `extern-c` and the erased-word case, neither with a repro.

## What was fixed first, and what this was the residue of

Regions (RM3, on by default since 2026-09-05) rewind a generation when the
bracket's RESULT cannot reach it. That was the whole check, and three programs
showed it is not enough: a node `vec-push!`ed into an outer vec from inside
`with-region`, a node behind an erased `:int` field of an admitted record
result, and a `(Vec int)` of erased nodes each segfaulted on the default build
and printed the right answer under `TUR_REGIONS=0`.

The fix (`docs/archive/regions-plan.md`, "region-lock-hardening") widens the
runtime lock from the result word to every word that leaves the generation:
every store primitive the compiler knows about says `TUR_REGION_NOTE(word)`,
an erasing ascription notes the value it erases, and a by-value aggregate
result is noted by its words.

## The inline-C class -- FIXED 2026-09-06

This report originally said user inline-C "cannot [be hooked] without more
machinery" and proposed a documented contract. That was wrong twice over.

**It was worse than filed.** The typed variant is not exotic and needs no
`unsafe` ascription trickery:

```turmeric
(defn cell-set! [c : ptr<void> v : Link] : nil
  ```c
  *(int64_t *)c = (int64_t)(intptr_t)v;
  ```)
(defn stash [c : ptr<void> n : int] : int
  (with-region (fn [] (do (cell-set! c (Link n 0)) 1))))
```

The bracket's result is an `:int`, both locks clear it, the generation
**rewinds**, and the read after the pop printed
`-2387225703656530210` and **exited 0** -- a silent wrong answer on the default
build where `TUR_REGIONS=0` printed 7. The poison did not even trap: the pooled
arena had been reused and unpoisoned by a later allocation.

**And it did not need more machinery.** A call site cannot note the argument
without re-emitting it, but the *callee* can: in an emitted inline-C function
every parameter is already a plain C identifier. So the note goes at body
entry, once per function, and covers stdlib and user inline-C by one rule.

The filter is what makes it free (`emit_region_word_can_be_node`, emit_expr.c).
It asks "can this word BE region memory", which is much narrower than the
result lock's "cannot prove it reaches nothing" -- the latter also refuses a
`ptr<void>`, a `cstr`, a bare type variable and a `(Map K V)` handle, none of
which can ever be a node. Only a `:heap` ADT, or a by-value aggregate holding
one, qualifies; an opaque newtype (`defopaque BtCell :ptr`, a C-made handle)
and the malloc-backed collections are excluded by name. Getting that wrong
first cost 12,373 lines of emitted notes across the fixtures; getting it right
costs **zero** -- no snapshot in the tree changed, because nothing in it hands
a node to inline-C.

Pinned by `tests/fixtures/region-escape-via-inline-c` on both arms (which also
asserts the other direction: a bracket whose inline-C sees only scalars still
rewinds) and by the `store-inline-c` / `inline-c-scalar` cases in
`tests/regions-fuzz-src.py`.

## What remains open

1. **`extern-c`.** A foreign function declared with `(extern-c f [x :Link] ...)`
   has no emitted body, so there is nowhere to put the callee-side note, and
   the call site cannot note an argument without re-emitting it. No repro: the
   `extern-c` declarations in the tree take `:ptr`, `:int` and `:cstr`, none of
   which can be region memory, and a foreign function taking a Turmeric `:heap`
   node by its ADT type would be unusual. If one appears, the fix is a
   call-site note on the hoisted argument temp.

2. **A stdlib primitive that stores an ERASED word.** The parameter note keys
   on the type, so a primitive whose `val` arrives as `:int` (which is most of
   them -- `chan-send`, `schan-send`, `work-queue-push`, the `sized-*-set!`
   family, `json/array-push`, `schema/*`) is not covered by it. Those are
   covered instead by the erasure note at the `(:: node :int)` that produced
   the word, and the hottest of them carry an explicit `TUR_REGION_NOTE`
   already. The gap is a program that obtains an erased node without an
   ascription the emitter sees. No repro; an audit of the `!`-suffixed
   primitives is one line each if one appears.

3. **Latent: the top-level panic jam.** A panic caught by the outermost
   `catch-unwind` after unwinding through a bracket leaves that generation open
   with nothing left to pop it, so every later allocation lands in it (correct,
   never freed until exit). Not reachable today -- both catch paths close the
   bracket before the propagation check runs, verified by probe -- and a
   shallower pop now retires skipped generations, so only the outermost case is
   left. The fix, if ever needed, is for the catch boundary to record
   `tur_region_depth()` on entry and retire down to it on the panic arm.

## Fix directions

- Leave 1 and 2 until a repro exists; both are one small change at that point.
- Longer term, route the intermediaries a bracket creates (closure envs,
  element boxes) into the generation, so a node stored through one is
  region-to-region and the intermediary itself is what the note sees. That was
  RM3's original R2 direction and needs the free-side guard at every
  intermediary free site; it is not v1 work.
