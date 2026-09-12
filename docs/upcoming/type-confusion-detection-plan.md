# Catching scalar representation confusion: ratchet first, fuzz second

> **Status:** proposed (2026-09-11). **Track:** post-v1, but the F0 half is
> cheap enough to land any time.
> **Type:** test infrastructure -- one warning ratchet extension, three gaps
> closed in an existing fuzzer, and a seed-rotation story.
> **Sequencing:** before [lattice-vocabulary-plan.md](lattice-vocabulary-plan.md)
> and [crdt-spice-plan.md](crdt-spice-plan.md). Both of those are blocked on
> defects of exactly the class this plan detects, and neither should land
> vocabulary on top of a detector that cannot see its failure mode.

## 0. Summary

The recurring complaint is real: nearly every sweep turns up an int carrier
reaching a slot that wanted a float, or a pointer reaching a slot that wanted
an integer. Recent instances, all within two releases -- the `any` -> scalar
cast miscompiling in the JIT engine, the `Sym` ctor emitted taking `int64_t`
while its caller passed `const struct __tur_sym *`, eight Saffron `any`-seam
fixes, and the two defects filed while probing the lattice vocabulary
([nested-class-method-call-picks-the-first-instance](../archive/nested-class-method-call-picks-the-first-instance.md),
[nullary-class-method-unresolvable-over-newtype-tyvar](../archive/nullary-class-method-unresolvable-over-newtype-tyvar.md)).

The answer to "can this be fuzzed" is yes -- and the more useful answer is
that **it already is, by four fuzzers, and the cheapest missing detector is
not a fuzzer at all.** Three findings shape this plan:

1. **`-Wfloat-conversion` is not in `-Wall`** (verified: 0 warnings vs 2 on a
   `long f(double d){return d;}` canary). So the float half of this class is
   *unwatched* in emitted C today, exactly as the pointer/integer half was
   before `docs/archive/emitted-c-pointer-integer-warnings-unwatched.md`. That
   report's ratchet is still in `tests/run.sh`, and extending it is a
   one-pattern change.
2. **The four existing fuzzers all run in CI pinned to seed 1.** They are
   regression guards, not searches. Nothing rotates a seed anywhere in the
   tree, so they have already found everything they will ever find.
3. **`tests/type-fuzz-src.py` is aimed squarely at this class and still cannot
   reach it**, for three specific reasons readable in its generator (section
   3.1). This is a shape-space gap, not a design failure -- the harness is good
   and the fix is local.

## 1. What the defect class actually is

Not "a type error" in the surface language -- the checker accepts all of these.
The shape is: **a value's representation and its declared type disagree across
an erasure boundary.** `tur check` passes; then either cc rejects the emitted
C, or the linker misses a symbol, or the program runs and prints a wrong
number.

The wrong-number arm is the dangerous one. `7.1` arriving as `7` is a passing
test with a wrong answer, and it is invisible to every check the suite runs
unless something compares against a known-correct value.

`docs/guides/value-representations-guide.md` is the existing inventory of
representations and boundaries; `type-fuzz-src.py`'s header says to keep the
two in sync, and that instruction stands.

## 2. What already exists

### 2.1 Four source-level fuzzers, all correct-by-construction

| Harness | Domain | Lines |
| --- | --- | --- |
| `tests/type-fuzz-src.py` | typed-boundary plumbing (wrappers x crossings) | 930 |
| `tests/saffron-fuzz-src.py` | `#lang saffron` `any` plumbing | 919 |
| `tests/refine-fuzz-src.py` | refinement types | 1224 |
| `tests/regions-fuzz-src.py` | region brackets (RM3) | 546 |

This is genuinely good infrastructure and the plan does not propose replacing
any of it. Each one:

- **Generates programs that are correct by construction** and knows the exact
  expected stdout, so the oracle is total rather than differential-only:
  `tur check` accepting implies the C compiles, links, runs, and prints the
  predicted output.
- **Classifies failures** into `BUG_invalid_c` / `BUG_link` / `BUG_crash` /
  `BUG_wrong_output`, plus a report-only `GEN_REJECT` for "the generator
  claims this is legal and the checker disagrees."
- **Shrinks automatically** by re-running each independent "leg" alone.
- **Carries a known-open-bug table** so filed findings are excluded from
  generation and pinned by `--known-probes` instead -- a hit is therefore new.
- **Self-tests the classifier**, proving it can see each failure class.
- Runs as a ctest target (`tur_type_fuzz_src`, `tur_saffron_fuzz_src`,
  `tur_refine_fuzz_src`, `tur_regions_fuzz_src`), which CI executes in the
  `-E '^tur_tests$'` aux job.

The saffron one additionally carries an interpreter arm (`IBUG_wrong_output`),
which is what makes it a *differential* fuzzer rather than only a model one.

### 2.2 But every one of them is pinned to seed 1

```
tests/run-refine-fuzz-src.sh:30:  SEED="${REFINE_FUZZ_SEED:-1}"
tests/run-regions-fuzz-src.sh:30: SEED="${REGIONS_FUZZ_SEED:-1}"
tests/run-type-fuzz-src.sh:28:    SEED="${TYPE_FUZZ_SEED:-1}"
tests/run-saffron-fuzz-src.sh:29: SEED="${SAFFRON_FUZZ_SEED:-1}"
```

with smoke-size `N` (40 for the type fuzzer). Every CI run compiles the same
forty programs. That is a perfectly good **regression** guard -- it is how a
fixed bug stays fixed -- but it performs no search. The search only happens
when a person runs the driver by hand with a fresh `--seed`, which the run
scripts' own headers tell you to do ("For a real session, run the Python driver
directly with a larger --n and a fresh --seed"), and which nothing schedules.

**This is the single biggest gap and it costs almost nothing to close.**

### 2.3 The emitted-C warning ratchet, and its canary

`tests/run.sh:796` fails any fixture whose captured build stderr matches:

```
\[-W(int-conversion|incompatible-pointer-types|free-nonheap-object)\]
```

plus a clang-only UBSan check for `through pointer to incorrect function type`.
`tests/check-cc-warn-ratchet.sh` is a self-test that builds a program whose
emitted C provably mixes a pointer and an integer and asserts the pattern
fires -- because "a grep that silently matches nothing looks exactly like a
clean corpus," which had already produced two false-clean sweeps.

The procedure that established it is the one this plan copies: **sweep the
corpus to zero, then ratchet.** That report records "The corpus was
sweep-verified at ZERO before this landed (2563 fixtures, built with these
same flags), which is what makes FAIL affordable rather than a warning nobody
reads."

## 3. Why the existing machinery missed the two new defects

### 3.1 `type-fuzz-src.py`'s typeclass shape space has three gaps

Its two typeclass crossings are generated like this (`x_class_thru`, line 525,
and the `class_extract` variant at line 307):

```python
cls, meth = "FzT%d%d" % (self.i, self.n_names), self.name("p")
leg.defs.append("(defclass %s [a] (%s [self : a] : a))" % (cls, meth))
leg.defs.append("(definstance %s [%s] (%s [self : %s] : %s self))"
                % (cls, tn, meth, tn, tn))
```

Three properties follow, and each one independently makes
[defect 1](../archive/nested-class-method-call-picks-the-first-instance.md)
unreachable:

1. **One instance per class.** The class name is freshly generated per
   crossing, so every class has exactly one instance. "Resolve to the first
   declared instance" and "resolve correctly" are then the same behavior. The
   bug is invisible *by construction*, not by luck.
2. **Unary methods only.** The generated method is `[self : a] : a` --
   identity-shaped. A binary method whose result has the same type as its
   arguments (`combine : a -> a -> a`) is what allows one call to feed
   another, and it is never generated.
3. **No same-class nesting.** Crossings compose, but each hop introduces a
   fresh class, so a method result never lands in the receiver position of the
   *same* class's method.

Defect 1 requires all three of: a constrained generic body, a nested
same-class call, and a non-first instance. The generator supplies none of
them.

[Defect 2](../archive/nullary-class-method-unresolvable-over-newtype-tyvar.md)
is missed for a fourth reason: no generated method is **nullary**. Every method
takes `self`, so a method whose class variable appears only in the return type
is outside the space entirely.

### 3.2 The float half of the emitted-C class is unwatched

Verified on Apple clang 21:

```
$ printf 'long f(double d){return d;}\n' > w.c
$ cc -fsyntax-only -Wall w.c                      # 0 warnings
$ cc -fsyntax-only -Wall -Wfloat-conversion w.c   # 2 warnings
```

`-Wfloat-conversion` is not implied by `-Wall`, and `tests/run.sh`'s
`TUR_CC_FLAGS` is `-O2 -std=c99 -Wall -Werror=implicit-function-declaration
-fno-strict-aliasing`. So the flag has never been on, and the ratchet cannot
match a warning that was never emitted.

On defect 1's repro it is a **precise** detector -- two warnings, both on the
miscompiled line, and zero on an equivalent correct float program:

```
bug.c:9041:58: warning: implicit conversion turns floating-point number into
  integer: 'double' to 'int64_t' [-Wfloat-conversion]
```

## 4. The plan

### F0 -- ratchet `-Wfloat-conversion` (no fuzzing required)

1. Add `-Wfloat-conversion` to `TUR_CC_FLAGS` in `tests/run.sh`.
2. **Sweep the corpus and drive it to zero** before ratcheting, per the
   established procedure. *(Sweep result: see section 5.)*
3. Add `float-conversion` to the ratchet pattern at `tests/run.sh:796`.
4. Add the matching canary to `tests/check-cc-warn-ratchet.sh` -- a program
   whose emitted C provably converts a double to an integer slot -- since the
   whole point of that file is that an unfired grep is indistinguishable from a
   clean corpus. **Do not skip this step**; it is the lesson the file exists to
   record.

Portability notes for step 3: `-Wfloat-conversion` is understood by both GCC
(since 4.9) and clang and spells its diagnostic tag the same way, so the grep
is portable. `-Wimplicit-int-float-conversion` is **clang-only** -- include it
in the flags if desired, but do not put it in the ratchet pattern without a
GCC-side equivalent, or the Linux and macOS legs will disagree about what
fails. Avoid `-Wconversion` wholesale: it brings roughly seven additional
warnings out of the hand-written preamble and runtime (`-Wshorten-64-to-32`,
`-Wsign-conversion`), which would have to be cleaned first.

This phase catches the emitted-C half of the entire defect class, costs one
flag and one canary, and is independent of everything below.

### F1 -- close the four gaps in `type-fuzz-src.py`

Local changes to the generator, each mapping to a gap in 3.1:

1. **Multiple instances per class.** Generate a class once per program (not
   per crossing) and give it instances at two or more of the leg's types, so
   "first declared" and "correct" can diverge. This alone makes defect 1
   reachable.
2. **Binary same-type methods.** Add a `combine`-shaped method
   (`[x : a y : a] : a`) alongside the existing identity shape.
3. **Same-class nesting.** Allow a class-method result to land in the receiver
   position of the same class's method.
4. **Nullary methods.** Add a `[] : a` shape, and generate instances over
   `defopaque` newtypes as well as plain type names -- which is defect 2's
   exact shape.

Both new defects get rows in the known-bug table so they are excluded from
generation and pinned by `--known-probes` until fixed -- the harness's existing
discipline, not a new one.

The float-specific value here: with two instances at `int` and `float`, a
wrong-instance resolution produces `BUG_wrong_output` against the generator's
predicted value. That is the oracle that catches the wrong-number arm, which
F0's static check cannot see when both instances happen to share a carrier.

### F2 -- run the search, not just the regression

The fuzzers need a seed that moves. Two changes:

1. **A scheduled job** (nightly, or weekly) that runs all four drivers
   directly with a large `--n` and a seed derived from the date, and opens an
   issue or writes a report on a non-`ok` classification. Not on the PR path --
   a fuzz hit is a finding to triage, not a reason to block a merge, and the
   project's standing position is that a red suite never blocks landing.
2. **A corpus of seeds that once found something**, replayed at smoke size on
   every run. This is what makes a fixed bug stay fixed without pinning the
   search to seed 1 forever.

Leave the ctest targets exactly as they are: seed 1, smoke size, on every
build. They are doing their job; they are just not the search.

### F3 -- metamorphic int/float retyping (optional)

The CLAUDE.md rule "always pick a literal with a non-zero fractional part"
exists precisely because this class is endemic. A metamorphic oracle automates
it: take a generated program that routes an `int`, mechanically retype it to
`float` with values that are exact in binary and have non-zero fractional
parts, and assert the two runs agree modulo the substitution.

This needs no second implementation and no predicted output -- the original
program *is* the oracle. Worth doing only if F1 proves insufficient; the
correct-by-construction generator already knows the expected value, which is
strictly stronger.

## 5. The sweep -- method and result

Run 2026-09-11 against `build/tur` at v0.46.1 (Debug, macOS arm64, Apple clang
21), the full compiled-fixture suite with the flag added:

```sh
TUR_CC_FLAGS="-O2 -std=c99 -Wall -Wfloat-conversion \
  -Wimplicit-int-float-conversion -Werror=implicit-function-declaration \
  -fno-strict-aliasing -L./build/src" bash tests/run.sh
```

**Result: 0 hits.** `summary: 2945 passed, 1 failed`, and neither flag appears
in any of the 2250 captured `actual.stderr` files. (2250 < 2945 because
interpreted-path and skipped fixtures never invoke cc; the sweep covers exactly
the fixtures that do, which is also exactly the set the ratchet would police.)

The single failure is unrelated and pre-existing:
`errors/expr-nesting-depth-limit (no runnable input)` is an **empty directory
holding only gitignored artifacts** (`actual.stderr`, `jit.stderr`) left behind
when `8039ae53a` retired the depth cap and deleted the fixture's inputs. Zero
files there are tracked, so a fresh clone does not see it; the harness even
prints the remedy ("If this dir holds only generated artifacts ... delete the
directory"). Worth a one-line cleanup, unrelated to this plan.

### The detector was armed -- proof, not assumption

The precedent report's entire lesson is that "a grep that silently matches
nothing looks exactly like a clean corpus," and that two earlier sweeps
reported false zeros. So the zero above is only meaningful alongside a positive
control. Building
[defect 1](../archive/nested-class-method-call-picks-the-first-instance.md)'s
repro through `tur build` with **the same `TUR_CC_FLAGS`** the sweep used:

```
$ CC=cc tur build bug.tur -o bug.bin
.../tur-build/bug_tur.c:7784:58: warning: implicit conversion turns
  floating-point number into integer: 'double' to 'int64_t' [-Wfloat-conversion]
.../tur-build/bug_tur.c:7784:48: warning: ... same line ...
```

Three matching warnings. The flag reaches the emitted C through the real build
path, and the corpus is clean under it.

**So F0 costs one flag, one pattern edit, and one canary -- there is no
cleanup backlog to pay down first.** This is the cheapest item in any of the
three sequenced plans, and it is the one that converts the recurring
int-into-float-slot class from a silent wrong answer into a suite failure.

## 5a. Execution -- F0, F1, F2 LANDED 2026-09-11

Verified against `build/tur` at v0.46.1 (Debug, macOS arm64, Apple clang 21).

### F0 -- the ratchet is live

- `tests/run.sh`: `-Wfloat-conversion` added to `TUR_CC_FLAGS`, with the
  rationale and both deliberate exclusions (`-Wconversion`,
  `-Wimplicit-int-float-conversion`) recorded at the flag.
- `tests/run.sh`: `float-conversion` joined the ratchet pattern at both grep
  sites; the FAIL line now reads "confuses scalar representations
  (pointer/integer/float)" and the result reason is
  `emitted C representation warning`.
- `tests/check-cc-warn-ratchet.sh`: a float canary, plus **two** disarm checks,
  because this arm can be silenced two ways that the pointer arm cannot.

Two traps surfaced while building the canary, both worth keeping:

1. **`return 7.5;` is the wrong canary.** A constant conversion is tagged
   `-Wliteral-conversion`, a *different* flag the ratchet does not match, so
   the obvious canary would pass while proving nothing. The canary returns a
   `float` **parameter** from an `:int` defn instead.
2. **The first disarm check was itself a false clean.** Grepping the whole of
   `tests/run.sh` for `-Wfloat-conversion` matched the new *comment block*, so
   deleting the actual flag still passed. It is scoped to the
   `^export TUR_CC_FLAGS=` line now. This is the same failure this script
   exists to prevent, reintroduced one level up -- exactly as its header warns.

The env-based check drafted first was also wrong: `run.sh` calls the canary at
line 141 but exports `TUR_CC_FLAGS` at line 219, and `tur_cc_warn_ratchet` runs
it standalone with no environment at all, so an env probe is vacuous in both
places it actually runs.

Verified in all three disarm modes: standalone/ctest exits 0; an env override
dropping the flag exits 1; deleting the flag from `run.sh` exits 1.
**Full suite after: `2945 passed, 1 failed`** -- the same pre-existing failure
as the baseline sweep (since removed: it was an artifact-only directory with
zero tracked files, left by `8039ae53a`).

### F1 -- the four shape gaps are closed

`tests/type-fuzz-src.py` gained two crossings:

- `x_class_nested` closes gaps 1-3 in one shape: a class with **two**
  instances (the leg's own type declared second), a **binary** same-type
  method, and a **nested** call inside a constrained generic. The method
  projects its first argument so the nesting is an identity -- a wrong
  instance therefore surfaces as `BUG_wrong_output`, a wrong *answer*, which
  is the arm F0's static check cannot see.
- `x_class_nullary_newtype` closes gap 4: a **nullary** method over a
  `defopaque` newtype, on bare `int` legs.

Both are open reports, so both are excluded by default via `known_bug_slug`
and pinned in `KNOWN_PROBES`, per the harness's existing discipline.

**`known_probes()` needed a fix to make that pinning honest.** It decided
"fires" from `out.kind in (crash, invalid_c, link, reject, other)` -- which
never includes a wrong ANSWER, since such a program exits 0 with
`kind == "clean"`. Defect 1's probe would have printed
`FIXED -- retire its known_bug_slug row` on a still-broken build. `KNOWN_PROBES`
rows may now be `(label, src, expected)` and a clean run whose stdout differs
counts as firing:

```
nested-class-method-call-picks-the-first-instance   fires (wrong_output: '7\n' != '7.1\n')
nullary-class-method-unresolvable-over-newtype-tyvar fires (reject)
```

Verified: `--self-test` PASS; default mode 60/60 ok with 0 BUG (shapes
excluded); `--emit-known --n 120 --seed 7` generates both and classifies them
`KNOWN(...)` with 0 BUG.

**One finding worth propagating.** `class_nested` fires on `cstr`, `bool` and
`int` legs as readily as on `float` -- because the generator declares a decoy
instance first, where the hand repro happened to declare `int` first. The
defect is wrong-instance selection generally; truncation is only its most
legible symptom. The report's condition 3 has been corrected accordingly.

### F2 -- the search now moves

- `.github/workflows/fuzz.yml`: nightly (04:30 UTC) plus `workflow_dispatch`,
  running all four drivers at `--n 400` with a date-derived seed. **Not** on
  push or pull_request. Every harness runs even after an earlier one hits, so
  one finding does not waste the other three searches. Findings upload as an
  artifact and open a labelled issue rather than a red X -- nothing here gates
  a merge, and a failing scheduled check is a signal people stop reading.
- `tests/fuzz-seed-corpus.txt` + `tests/replay-fuzz-seeds.sh`: every
  `run-*-fuzz-src.sh` now replays the seeds recorded for it after its smoke
  run. The corpus is empty until the nightly runs, and an empty corpus **says
  so** rather than passing silently. Verified on both paths: a recorded seed
  replays, a malformed row is reported and skipped, another harness's row is
  ignored.

All five affected ctest targets pass: `tur_cc_warn_ratchet`,
`tur_type_fuzz_src`, `tur_saffron_fuzz_src`, `tur_refine_fuzz_src`,
`tur_regions_fuzz_src`.

### F3 -- deliberately NOT done

F3 is conditional on F1 proving insufficient ("the correct-by-construction
generator already knows the expected value, which is strictly stronger"). F1
reaches both defects and produces a wrong-answer verdict on the first, so the
condition is not met. Left in section 4 as the escape hatch it was written as.

## 6. Risks and open questions

- **The sweep may not be zero.** If fixtures already trip
  `-Wfloat-conversion`, each is either a real latent defect or a deliberate
  narrowing that needs an explicit cast in the emitter. Cleaning them is F0's
  real cost and cannot be estimated before the sweep runs.
- **A ratchet that fires on legitimate narrowing is worse than none.** Turmeric
  has `cast`, and a deliberate float-to-int cast must not fail the suite. If
  the emitter lowers `cast` to an implicit conversion rather than an explicit
  C cast, F0 requires an emitter change first -- and that is itself worth
  knowing.
- **F1 could surface a burst of findings.** Widening a fuzzer's shape space
  usually does. That is the point, but it means F1 should land when someone has
  time to triage, and its new shapes should go into the known-bug table rather
  than block the merge.
- **GCC/clang divergence in the ratchet pattern.** Covered in F0, but worth
  re-checking on the Linux leg specifically; the macOS leg is AppleClang 21,
  where three warnings are already hard errors.
- **None of this addresses shared-elaboration bugs.** The differential arm
  (compiled vs `--interpret`) is blind to anything wrong in code both back ends
  share, and the model oracle only checks what the generator thought to
  predict. Neither is a complete story; both are much better than the current
  seed-1 status quo.

## 7. References

- Precedent: `docs/archive/emitted-c-pointer-integer-warnings-unwatched.md`
  (the sweep-then-ratchet procedure this plan copies), and
  `docs/archive/emitter-thunk-type-return-mismatch.md` (the function-pointer
  ratchet beside it).
- Blocked-on consumers: [lattice-vocabulary-plan.md](lattice-vocabulary-plan.md),
  [crdt-spice-plan.md](crdt-spice-plan.md).
- Defects this plan would have caught:
  [nested-class-method-call-picks-the-first-instance](../archive/nested-class-method-call-picks-the-first-instance.md),
  [nullary-class-method-unresolvable-over-newtype-tyvar](../archive/nullary-class-method-unresolvable-over-newtype-tyvar.md).
- In-tree: `docs/guides/value-representations-guide.md` (the representation and
  boundary inventory the type fuzzer walks).
