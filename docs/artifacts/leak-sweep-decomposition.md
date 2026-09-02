---
title: What the corpus leak sweep actually contains
category: Artifact
description: The 8.3 KB the RM1 measurement surfaced is four unrelated classes, only one of which is RM1's. Two are product defects (one newly filed), one is RM2's, and the largest single entry is fixture scaffolding. Recorded so the headline number is not quoted as one problem.
---

# Corpus leak sweep -- decomposition

Swept 2026-08-30 (v0.42.2 + this branch) while looking into the leaks the
[RM1 measurement](../upcoming/reclamation-plan.md) surfaced. The headline
"8.3 KB of leaks in emitted programs" is **four unrelated classes**, and
reading it as one number overstates the product problem in one direction while
hiding a real default-path defect in the other.

| class | example | bytes | whose problem |
|---|---|---:|---|
| fixture scaffolding | `httpd-req-string-opt`'s `fake-conn-cookie` / `fake-conn-body` | ~3.5 K | **nobody's** -- the fixture's own `calloc`'d `HttpdConn` and `strdup`'d headers, never freed by the test |
| fixture-owned runtime values | `option-niche-crossings` (`vec_new`, `vec_push_ex`, `tur_string_from_bytes`) | ~0.4 K | **nobody's** -- Vecs and Strings the fixture allocates and never `vec-free`s |
| recursive sum spine | `re-string`'s `ctor_RxCons` / `ctor_REmpty` / `ctor_RxNil` | ~0.6 K | **RM2** (the recursive spine), not RM1 |
| **value-struct payload box** | `rational-*`'s `ok__spec__...` / `err__spec__...` | 0.46 K -> 0.13 K | **a defect**, filed: [value-struct-payload-sum-monomorph-box-has-no-owner](../reported/value-struct-payload-sum-monomorph-box-has-no-owner.md); a scope-exit drop covers the let-bound shape, an inferred non-retaining sum-param mask (now admitting `cstr` results) plus a statement-position drain cover the argument shape, and the carrier free is deep for a boxed struct arm (7 of 9 fixtures clean); the remaining 2 are dictionary-dispatched inline-C class methods, which need the glue route |
| erased sum carrier | `hkt-*`, `result-basic` | ~0.7 K remaining | **RM1**, partially closed 2026-08-30; bind chains closed 2026-09-02 (sweep 7200 -> 6856 B; 704 B of SUM-BOX rows left, dominated by inline-C `:int` readers in fixtures and dictionary-dispatched sites) |

## The two things worth carrying forward

**The largest entry is not a compiler problem.** `httpd-req-string-opt` leaks
3756 bytes, and 3528 of them are the fixture's own C scaffolding building fake
`HttpdConn` structures. Any future "the corpus leaks N bytes" claim should
subtract this class first; otherwise a fixture that allocates more test data
looks like a regression.

**The smallest entry is the most serious.** The value-struct payload box is on the
DEFAULT path with no experiment involved, and it falsifies a narrowing claim
that was being relied on -- SR2a's "a concrete monomorph flows by value, so
there is no box to own" is true only for payloads of 8 bytes or less. It is
also the most tractable of the real classes, because the constructor mallocs a
fresh copy and the resulting monomorph is its sole owner: no freshness
analysis needed, unlike RM1's residue.

## Method

`benchmarks/adt-alloc/rm1-erased-base-sweep.sh` finds erased-base callers;
the wide-payload population comes from a static scan for a `*__spec__*` sum
constructor whose body mallocs (11 fixtures), confirmed by building each under
`-fsanitize=address` and requiring a `*__spec__*` frame in the leak trace
(9 of the 11). Per-fixture byte counts are best-of-one runs and are stable,
but they are per-EXECUTION totals for these specific fixtures -- they measure
presence and shape, not severity in a real program, where every one of these
is per-construction and grows in a loop.
