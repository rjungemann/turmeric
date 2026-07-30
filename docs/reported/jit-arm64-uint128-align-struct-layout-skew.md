# JIT on arm64 macOS: c2mir aligns `__uint128_t` to 8, skewing every struct that embeds `ucontext_t`

**Severity: HIGH.** Silent layout divergence at the JIT/host-runtime ABI
seam. The program's view of a runtime struct disagrees with the host
runtime's view, so field reads land on the wrong bytes. Observed as an
infinite hang (two fixtures), but the failure mode is general and can
equally produce a silent wrong answer.

Found 2026-07-30 replaying J3 on an Apple M2 (macOS 27.0), branch
`claude/j0-jit-engine-plan-znqibo`, originally at `aacb8c5bc` and since
rebased onto `5ef07d50a`. This is the first Apple Silicon run of anything
past `d3eed7d83`.

## Symptom

`tests/run-jit.sh` on M2, **pre-patch**: 2387 passed / 5 failed / 48
skipped, against a then-current Linux baseline of 2392 / 0 / 48. Both
figures predate section 30.3 teaching run.sh to descend into group
directories; the post-patch table further down is on the new harness and
is the one to compare against today's 2393 / 0 / 47.

Two of the five hang until the harness timeout:

- `async-await-channel`
- `fiber-scheduler`

Both pass under plain `tests/run.sh` on the same box with the same binary,
so this is JIT-path-only. Neither is an ASan artifact -- both reproduce on a
`-DTUR_DEBUG_SANITIZE=OFF` build.

`sample(1)` on the hung process shows a tight spin between JIT-generated
code and `tur_fiber_block_resume` (`src/runtime/generated/tur_rt_split.c:1938`,
`:1952`) -- the scheduler loop re-enqueues a fiber that never reports `done`.

## Minimal repro

```turmeric
(defn probe [] : int
  ```c
  struct S { char c; __uint128_t v; };
  fprintf(stderr, "size=%d align=%d | struct{char;u128}: size=%d offsetof(v)=%d\n",
          (int)sizeof(__uint128_t), (int)_Alignof(__uint128_t),
          (int)sizeof(struct S), (int)offsetof(struct S, v));
  return 0;
  ```)
(defn main [] : int (probe))
```

```
tur --enable=jit jit repro.tur    ->  size=16 align=8  | struct: size=24 offsetof(v)=8
tur build repro.tur -o r && ./r   ->  size=16 align=16 | struct: size=32 offsetof(v)=16
```

## Root cause

`c2mir` has no native 128-bit integer type. Its AArch64 target header
fakes one as a two-word struct:

    build/_deps/mir-src/c2mir/aarch64/mirc_aarch64_linux.h:141
    typedef struct {unsigned long hi, lo;} __uint128_t;

That gives size 16 but **alignment 8**. AAPCS64 (and clang) require
alignment 16. Note also that this directory ships only a `_linux` target
header -- there is no Darwin variant -- so macOS gets the Linux
assumptions verbatim.

The error then compounds through Apple's signal-context headers, which
embed a NEON register file of 128-bit lanes:

| type | c2mir | clang | delta |
|---|---|---|---|
| `_STRUCT_ARM_NEON_STATE64` | size 520, align 8 | size 528, align 16 | -8 |
| `_STRUCT_MCONTEXT64` | size 808, align 8 | size 816, align 16 | -8 |
| `ucontext_t` | size 864, align 8 | size 880, align 16 | -16 |
| `FiberBlock` | size 2000, `done` @ 1744 | size 2032, `done` @ 1776 | -32 |

`FiberBlock` embeds two `ucontext_t` by value (`ctx`, `caller_ctx`), so it
lands 32 bytes short.

The hang follows directly. `tur_fiber_shim` runs **host-resident** (it is
compiled into `tur` by the platform cc, via the S2 split runtime) and
writes `f->done = 1` at offset 1776. The fixture's inline C runs
**JIT-compiled** by c2mir and reads `((FiberBlock *)f)->done` at offset
1744. The write is never observed; `sched-run`'s
`while (sq->count > 0)` loop re-enqueues forever.

Confirmed end-to-end on the minimal case: the fiber body demonstrably
executes, and `done` still reads back 0 under JIT and 1 under AOT.

## Why this is the S2 seam

This is the hazard findings section 20.5 flagged -- Linux and macOS
diverge exactly where the host-resident runtime meets program-side code --
and it is the same *shape* as the section 20.3 `TaskGroupBlock` bug
(inconsistent struct layouts across a compilation boundary, invisible on
glibc). It is invisible on x86-64 Linux because nothing in glibc's
`ucontext_t` needs 16-byte alignment.

It generalizes past fibers: **any** runtime struct whose layout the host cc
and c2mir compute differently is affected, and any program-side inline C
that touches such a struct reads the wrong offset silently.

## Fix -- LANDED

A three-hunk patch to the MIR fork fixes it.

- Fork commit: `rjungemann/mir` `90633091` on
  `fix/make-one-ret-distinct-targets` (the fork's third local fix).
- `TUR_MIR_GIT_TAG` repointed at it in `cmake/mir.cmake`.

Verified from a **fresh** build directory, not the edited `_deps` tree --
`cmake/mir.cmake:33-41` warns that an existing build dir silently keeps
fetching its old pin, so re-verifying in place would have proved nothing.
`build-pin/_deps/mir-src` fetches `9063309` with a clean worktree, and all
three suites reproduce (findings 32.2 numbers) from it.

The patch has two independent parts.

**(a) c2mir ignores `_Alignas` on a struct member.** Two of the three sites
that compute member alignment read only the member's *type* alignment and
drop `decl_spec.align`. Note the zero-size-member branch at `c2mir.c:6267`
already does this correctly -- the normal paths just never got the same
line, so `_Alignas` on a member silently does nothing.

**(b) `spec_qual_list` cannot parse `_Alignas` at all.** Struct members go
through `spec_qual_list`, which handles only type-specifiers and
type-qualifiers; `_Alignas` is wired up only in `declaration_specs`
(`c2mir.c:4515`). C11's grammar does omit alignment-specifier from
specifier-qualifier-list, but C23 and both gcc and clang accept it, and
without it (a) is unreachable.

With both, the target header's fake 128-bit type can carry its own
alignment:

```diff
--- a/c2mir/aarch64/mirc_aarch64_linux.h
+++ b/c2mir/aarch64/mirc_aarch64_linux.h
-    "typedef struct {unsigned long hi, lo;} __uint128_t;\n"
+    "typedef struct {_Alignas(16) unsigned long hi; unsigned long lo;} __uint128_t;\n"

--- a/c2mir/c2mir.c
+++ b/c2mir/c2mir.c
@@ D (spec_qual_list) @@
-    if (C (T_CONST) || C (T_RESTRICT) || C (T_VOLATILE) || C (T_ATOMIC)) {
+    if (C (T_ALIGNAS)) { /* C23 / gcc+clang: alignment-specifier in a member decl */
+      P (align_spec);
+      op = r;
+    } else if (C (T_CONST) || C (T_RESTRICT) || C (T_VOLATILE) || C (T_ATOMIC)) {

@@ aux_set_type_align @@
           member_align = type_align (decl->decl_spec.type);
+          if (decl->decl_spec.align > member_align) member_align = decl->decl_spec.align;
           if (align < member_align) align = member_align;

@@ set_type_layout @@
           member_align = type_align (decl->decl_spec.type);
+          if (decl->decl_spec.align > member_align) member_align = decl->decl_spec.align;
```

Note `_Alignas(16)` sits on the *first* member, and `hi, lo` is split into
two declarators -- an alignment-specifier applies per declaration, so the
combined form would align both.

### Result

Every layout in the table above now matches clang exactly (`__uint128_t`
16/16, `ucontext_t` 880/16, `FiberBlock` 2032 with `done` at 1776), the
minimal repro reports `done=1`, and both fixtures produce byte-exact
expected output.

Full corpus on M2, Debug `-DTUR_DEBUG_SANITIZE=OFF` build (Apple clang):

| suite | macOS M2 | Linux baseline |
|---|---|---|
| `tests/run.sh` | **2479 passed / 0 failed** | 2478 / 0 |
| `tests/run-jit.sh` | **2393 passed / 1 failed / 47 skipped** | 2393 / 0 / 47 |

The run.sh `+1` is not a divergence -- findings 31.3's quoted 2478 predates
the `tco-named-let-nocapture-deep` fixture added in that same commit, which
passes on Apple Silicon.

`tests/run.sh` is now at exact parity. The single remaining JIT-corpus
failure is `httpd-new-pool-fail-drops-handler`, which is **not a JIT bug**:
run standalone it prints `built` under *both* AOT and JIT on macOS. The
fixture forces a bind conflict to make `httpd-new-pool` fail, but BSD
`SO_REUSEADDR` lets the second bind succeed where Linux refuses it. It is a
macOS socket-semantics gap in the fixture (and flaky under the harness,
since it passed the AOT suite in the same session).

cc fallbacks are 60 on macOS vs 47 on Linux; the +13 is exactly the Apple
SDK header residue of section 20.2, unchanged by this patch.

### Residual risk

`mirc_aarch64_linux.h` is the only aarch64 target header MIR ships, so this
also changes aarch64 *Linux*. That direction is correct -- AAPCS64 requires
16-byte alignment for 128-bit integers, so unpatched c2mir is wrong there
too -- but it is untested here; only x86-64 Linux and arm64 macOS were run.
The `spec_qual_list` change is additive and target-independent.

## Fix directions (original survey, kept for the alternatives)

1. ~~**Give c2mir a correctly aligned 128-bit type.**~~ DONE -- this is the
   landed fix above.
2. **Cheaper containment -- stop exposing `ucontext_t`-bearing structs to
   program-side C.** Field access on `FiberBlock` from a fixture's inline C
   is what makes the skew observable. Accessors that stay host-resident
   (`tur_fiber_block_done(f)` instead of `f->done`) would make the layout
   private to the host. This also removes a whole class of future
   divergence, not just this instance.
3. **Guard -- assert layout agreement at JIT startup.** Have the prelude
   emit a check that the program's `sizeof`/`offsetof` for the runtime
   structs it can see match values the host baked in, and hard-error
   rather than silently miscomputing. Cheap, and it would have caught this
   in minutes instead of a bisect.

Options 2 and 3 are independently worth doing regardless of 1, because
they convert a silent wrong answer into a loud failure.

## Not covered by this report

- `httpd-new-pool-fail-drops-handler` -- see above; a BSD `SO_REUSEADDR`
  gap in the fixture, not a JIT defect.
- `httpd-h4-keepalive`, `httpd-h6-routing` appeared to fail on macOS in an
  early run. They do **not**: both pass on a clean non-sanitized build.
  They were ASan artifacts of the Homebrew-LLVM build described below.

## Harness note (not a product bug, but it will burn the next person)

The 46-failure and 32-failure numbers a first run produces on macOS are an
artifact of compiler mismatch, not of the product. If `tur` is built with
Homebrew LLVM (the documented workaround for the stale-ASan startup
deadlock, CLAUDE.md) but fixtures link with Apple's system `cc`, every
fixture that pulls in an ASan-instrumented `libturi.a` fails to link:

    Undefined symbols for architecture arm64:
      "___asan_version_mismatch_check_v8", referenced from:
          _asan.module_ctor in libturi.a[89](fiber.c.o)

Either pin the fixture compiler to the same toolchain that built
`libturi.a`:

    CC=/opt/homebrew/opt/llvm/bin/clang bash tests/run-jit.sh

or -- better, and what the numbers above use -- build unsanitized with
Apple clang, which sidesteps both the mismatch and the ASan startup
deadlock:

    cmake -S . -B build-nosan -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON \
          -DTUR_DEBUG_SANITIZE=OFF
    cmake --build build-nosan -j     # NOT --target tur: fixtures link
                                     # libturi.a, so build all targets

The pinned-`CC` route still leaves two spurious httpd failures that the
unsanitized build does not have. Also note `--target tur` alone does not
produce `libturi.a`, and every fixture then dies with
`ld: library 'turi' not found` -- which the harness reports as
`build failed`, i.e. it looks exactly like a compiler regression.
