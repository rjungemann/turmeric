---
title: defopaque over a pointer -- a pointer C spelling -- gate results
category: Planning
description: What happened when `(defopaque T :ptr<void>)` c-named as `void *` instead of `int64_t` behind a seam -- the compiler side is eight small edits and lands green, `(Option String)` takes the SR3 niche as predicted, and the whole remaining cost is ~61 stdlib inline-C blocks that still say `int64_t` about a pointer handle.
---

# `defopaque` over a pointer -- a pointer C spelling: gate results

> **GRADUATED 2026-08-28, the same day.** The recommendation at the bottom was
> followed exactly: the seam landed first (default off, corpus green both ways),
> then the migration landed and the default flipped, and the seam is gone --
> `adt_opaque_c_names_as_pointer` is now unconditional. Suite 2712/0.
>
> The migration came in at **66** inline-C returns rather than the ~61 estimated
> here, across the same 21 files, plus `panic.tur`'s conditional return and four
> fixtures. `stdlib/string.tur` needed nothing, as predicted.
>
> **What the estimate missed, and it is the interesting part: six EMITTER sites,
> not zero.** "Compiler: done" was true of the seam and wrong about graduation.
> Once the spelling is real for every pointer opaque -- not just the ones a
> default-off probe reached -- a producer and a consumer can disagree about
> which name the word goes by, and six places had to be taught the relabel:
> ascription, the function signature (definition and its forward-decl mirror),
> call arguments, the generic-base ctor slot, the spec field read, and the CPS
> clone-frame env shim. Each is one cast; none was visible from the seam-on
> corpus, because the seam only exercised the subset of pointer opaques the
> fixtures happened to route that way.
>
> Step (3) -- re-run the SR3 slice B gate -- was also done, and the
> inline-C carrier-builder hole this document predicted is exactly what it
> found. Slice B is unshelved as `--enable=option-niche`; see
> [sr3-option-niche-plan.md](sr3-option-niche-plan.md).

Follow-up (1) from
[sr3-slice-b-gate-results.md](sr3-slice-b-gate-results.md), which shelved SR3
slice B and named this as the better lever:

> **Give `defopaque` over a pointer a pointer C spelling.** Independently worth
> doing; makes `String` eligible; is the whole census.

Run 2026-08-28 by the same method the SR1/SR2/SR3/SR4 gates used: force the
representation behind a default-off seam, run the corpus, count the crossings
and the blockers before writing any of the phase.

**Verdict: the claim holds, and it costs less on the compiler side than the SR3
write-up implied.** The compiler is EIGHT small edits and the corpus is green
under the seam. `(Option String)` takes the SR3 niche with both seams on, with
correct output. What remains is not compiler work at all: **~61 inline-C blocks
in 21 stdlib files that return an `int64_t` where the handle is now a pointer.**
That is mechanical, and `stdlib/string.tur` -- the module the whole census is
named after -- needs **zero** changes, because it was already written against
`:ptr<void>` with `::` rather than an int64 cast.

## The seam

`TUR_OPAQUE_PTR_CNAME=1` (env-only, default off, the SR1/SR2/SR3/SR4
precedent). In the tree and working end to end.

| # | site | change |
|---|---|---|
| 1 | `AdtDef.opaque_base_is_ptr` (types.h) | the declared base was **not recorded at all** -- see below |
| 2 | `elab_defopaque` (elab_structs.c) | set it from the base keyword (`ptr`, `ptr-void`, `ptr<...>`) |
| 3 | `adt_opaque_c_names_as_pointer` (types.c) | the seam-gated predicate |
| 4 | `type_c_name` TY_ADT + TY_APP arms | a pointer opaque spells `void *`; a PARAMETRIC one (`(Goal int)`) must spell what bare `Goal` does |
| 5 | `repr_of` (types.c) | `REPR_SCALAR_BITS` -- a leaf pointer, the `cstr`/`ptr<T>` form, not a heap handle |
| 6 | `type_uses_carrier_abi` (emit_core.c) | a pointer opaque is not carrier-ABI, parametric or not |
| 7 | `emit_carrier_bridge` (emit_core.c) | the carrier crossing is a pure reinterpret, exactly as the `:heap` arm beside it already says |
| 8 | `field_byval_unbox` (emit_expr.c) | `!= "int64_t"` was standing in for "is an aggregate"; ask `strchr(cty, '*')` instead |

### The declared base type was being thrown away

`elab_defopaque` located the base-type form only to know where the trailing
`:linear` / `:affine` / `:sealed` attributes start, and never read it. So
`(defopaque String :ptr<void>)` and `(defopaque UserId :int)` were
**indistinguishable downstream** -- there was no field on `AdtDef` recording
which one you wrote. The `:ptr` / `:ptr<void>` in all 38 of stdlib's
pointer-opaque declarations was decoration.

That is worth landing on its own merits, seam or no seam: an annotation the
compiler parses for position and discards is one a reader reasonably believes is
load-bearing, and this is the "lazy `:int` stand-in" the CLAUDE.md rule is about
-- one level down, in the representation rather than the signature.

## Results

Corpus: 2712 fixtures, `bash tests/run.sh`, Debug build, 4-core box.

| run | result |
|---|---|
| seam OFF (all edits in tree) | **2712 passed, 0 failed** -- provably inert |
| `TUR_SR3_OPTION_NICHE=1` alone | **2712 passed, 0 failed** -- the allowlist row added below is inert without the pointer spelling |
| seam ON | 2560 passed, 152 failed |
| seam ON, `TUR_SKIP_CC_WARN_CHECK=1` | **2707 passed, 5 failed** |
| both seams, `TUR_SKIP_CC_WARN_CHECK=1` | 2706 passed, 6 failed |

The gap between rows 3 and 4 is the whole finding. **149 of the 152 failures are
one ratchet**, `tests/run.sh`'s emitted-C pointer/integer check
(`-Wint-conversion`), and **1288 of the 1300 warning lines are literally one
shape**:

```
warning: returning 'long int' from a function with return type 'void *'
         makes pointer from integer without a cast
```

-- an inline-C body ending `return (int64_t)(intptr_t)p;` in a function whose
return type is now `void *`. Not a compiler defect; source that says `int64_t`
about something the compiler now agrees is a pointer.

With the ratchet muted the seam runs **2707/5**, and the five are:

- **4 codegen (snapshot) mismatches** -- `inline-c-result-builder`,
  `inline-c-typed-result-option`, `map-multiword-struct-value`,
  `scheduler-multithread`. Expected of a representation change; regenerated at
  graduation, not now (with the seam off the snapshots are unchanged, which is
  what the 2712/0 row means).
- **1 fixture-source breakage** -- `barrier-rendezvous` hand-writes
  `extern bool barrier_hywait(int64_t);` inside its own inline-C. One line, and
  the right kind of failure: a hard `conflicting types` error at the C compiler,
  never a silent one.

**Zero stdout mismatches under the seam alone.** Every value that flows is
correct; what is red is the C compiler objecting to the spelling at the inline-C
boundary.

## The payoff, verified

`(Option String)` becomes niche-eligible, which was the point. `String` and
`StringBuilder` are added to `sr3_payload_is_nonnull_pointer`'s allowlist -- and
the row is **inert on its own**, because the rule's second half ("the payload
must c-name as a real POINTER") keeps them out unless `TUR_OPAQUE_PTR_CNAME` is
also on. With both seams, `httpd-req-string-opt` emits no
`tur_adt_Option__String` typedef at all:

```c
static void * ctor_None__String()            { return (void *)0; }
static void * ctor_Some__String(void * _0)   { return _0; }
```

and prints its expected output. Against the SR3 census -- `env` (5 spellings),
`httpd-string` (5), `args` (2), `re` (2), `docstrings` (2), all `(Option
String)` -- that is the entire non-`Cons` half of the population, exactly as
predicted.

### One new hole in slice B that only String could expose

Both seams together are 2706/6: the extra failure is
`inline-c-option-byval-param`, and it is a **silent wrong answer** (prints
blank where it should print `hi`), not a build error.

An inline-C body declared `: (Option String)` builds its result with the
preamble's typed builders:

```c
if (n == 0) return tur_none();
return tur_some_ptr(tur_string_from_bytes("hi", 2));
```

`tur_some_ptr` returns the **carrier** -- a pointer to a tagged box. Under the
niche the consumer reads that word as the payload String and hands the box
pointer to `string/to-cstr`. `tur_none()`'s `0` happens to be right; `Some` is
not.

This is slice B's gap, not the spelling's: the niche<->carrier bridge
(`emit_carrier_bridge`, the row SR3 slice B added) covers Turmeric-side
crossings and does not see an inline-C body producing the carrier form at a
`return`. No `Vec`/`Map`/`Set` fixture did that, which is why slice B's gate did
not find it; `String` is the payload people actually build in inline-C. The fix
direction is the same inverse the bridge already spells (`p ? tur_opt_value(p) :
0`), applied at the inline-C return boundary -- but it belongs to whoever
un-shelves slice B, and it is a reason to un-shelve it carefully.

## What graduation would cost

**Compiler: done.** The eight sites above, none of them large, and the seam runs
the corpus green.

**Source: ~61 inline-C blocks, 21 files, mechanical.**
`return (int64_t)(intptr_t)X;` becomes `return (void *)X;`. Distribution:

| n | file(s) |
|---|---|
| 15 | `stdlib/future.tur` |
| 10 | `stdlib/typeclass-show.tur` |
| 6 | `stdlib/threadpool.tur` |
| 4 | `stdlib/arc.tur` |
| 3 | `stdlib/io.tur`, `stdlib/time.tur`, `stdlib/trail.tur` |
| 2 | `stdlib/chan.tur`, `stdlib/serial.tur`, `stdlib/taskgroup.tur` |
| 1 | `stdlib/atomic.tur`, `barrier`, `condvar`, `dynvar`, `fiber`, `mutex`, `panic`, `reactor`, `rwlock`, `stm`, `thread` |

Plus the one `extern` line in `tests/fixtures/barrier-rendezvous`, and the 4
codegen snapshots.

Notably **`stdlib/string.tur` is not on that list.** Its constructors are
`(:: (tur_string_adopt_cstr s) String)` over `ptr<void>`-typed externs, so both
spellings agree and nothing changes. The module the census is named after was
already written the way the rest should be -- which is also the migration's
style guide.

**Compatibility.** This is a breaking change for user inline-C over a pointer
`defopaque`, and deliberately a LOUD one: every instance is a
`-Wint-conversion` the suite's ratchet already fails on, or a hard
`conflicting types` at a hand-written `extern`. There is a compat option if
that is judged too sharp -- emit the inline-C body into an `int64_t`-returning
static helper and have the pointer-spelled wrapper cast -- which would take the
61 edits to zero at the cost of a (trivially inlined) call layer. It is not
recommended: the whole point is that a handle over a pointer should say so.

## Recommendation

**Graduate it, in two commits.** The measurements support it and the ordering is
forced:

1. **Land the seam** (this change). Default off, corpus green both ways, and it
   ends the state where a `defopaque`'s declared base type is parsed and thrown
   away.
2. **Migrate the ~61 inline-C blocks and flip the default.** Mechanical, one
   sweep, snapshots regenerated in the same commit.
3. **Then re-run the SR3 slice B gate.** Its census becomes eligible, and the
   inline-C carrier-builder hole above is the first thing that gate should
   assert on -- it is the shape SR3's own soundness argument (a niche value must
   be *distinguishable* from a carrier value) predicts, arriving from the one
   direction the type system does not police.

Item (2) of the SR3 recommendation -- deciding what a `:heap` collection's empty
value is, which is what would make `Cons` eligible -- is untouched by this and
still costs what it cost.

## Regression cover

None added, following the SR3 precedent: the seam is default-off and provably
inert (2712/0 with it off, and 2712/0 with the SR3 seam alone). Reproduction is
one env var. If the default flips, the ratchet in `tests/run.sh` is already the
harness this needs -- it is what caught all 149 of these, by name, on the first
run.
