# Compile the runtime preamble once on the cc path

**Status: feasibility PROVEN end to end, not implemented.** A split program
compiles, links against a prebuilt runtime object and produces the correct
answer. What remains is build plumbing, not discovery.

**Expected win: ~17% of suite wall-clock.** Measured, and materially smaller
than the ~45% first estimated -- that figure was compile-*only* and taken while
the suite was running. Read the numbers below before deciding this is worth it.

## The waste

Every fixture's emitted TU carries the fixed runtime preamble, and `tur build`
hands the whole thing to `cc`:

```
emitted C for a ONE-LINE program:  8310 lines
  of which fixed runtime preamble: 4360 lines   (byte-identical every time)
```

2816 fixtures each recompile those 4360 lines. Nothing caches it: a single `cc`
call that compiles *and* links is uncacheable, which
[tur-link-and-build-split-plan](../archive/tur-link-and-build-split-plan.md)
already measured as "48/48 Uncacheable, 0 hits". That plan split compile from
link and added `--runtime=lib`, but `--runtime=lib` swaps the *autolinked
runtime sources*; the preamble is still inline in every program TU.

## Measured, idle box, gcc 16 / UCRT64

| TU handed to cc | lines | compile+link |
| --- | --- | --- |
| full (today) | 8310 | 1.31s |
| decls + program, prebuilt runtime | 5504 | 0.94s |

**29% off the `cc` call.** A fixture costs ~2.2s end to end (`tur` 0.5s, `cc`
1.3s, run + harness ~0.4s), so this is **~17% of the suite** -- roughly 30 min
to 25 on the Windows CI leg, and proportionally everywhere else.

That is worth having and is not a transformation. If the goal is a big cut, the
next-largest item is `tur` itself at 0.5s per fixture for a one-line program,
which is stdlib elaboration and a separate investigation.

## Proven

```sh
# runtime, compiled ONCE
gcc -O2 -c -o rt_split.o src/runtime/generated/tur_rt_split.c -Isrc -Isrc/runtime
gcc -c -o sjlj.o src/async/tur_sjlj_x64_win.S            # Windows only

# program half + prebuilt runtime
gcc -O2 -o t.exe split.c rt_split.o sjlj.o -lturt_runtime -L... -lm -lpthread ...
./t.exe   # -> correct output
```

The S2 split already produces exactly the text needed: `jit_try_split_preamble`
swaps the emitted preamble for `tur_rt_split_decls`, and the same swap applied
to `tur build`'s buffer is the whole change on the emitter side.

## The three obstacles, and what each needs

**1. `tur_closure_headers_enabled` is defined in both halves.** The generator
externs `static` globals in the decls half (`gen-runtime-split.py`, the
`extern {head_no_init}` branch) but this one is *non-static*, so it falls to
"keep verbatim in both" and collides at link.

Do **not** fix it by externing it in the decls half. `jit_sync_config_globals`
(`src/jit_engine.c`) locates it as a MIR **data item** in the program module and
copies its value onto the host's weak copy; with no definition there is no data
item and that handshake silently stops working. The JIT would keep building and
quietly use the wrong value.

Fix instead in the *impl* half: emit non-static global definitions with
`__attribute__((weak))`. The program half keeps its strong definition, so it
wins on the cc path and the JIT is untouched.

**2. `tur_sjlj_set` / `tur_sjlj_jump` are unresolved.** On the JIT path they
come from `JIT_SHIMS`; a cc-path link needs `src/async/tur_sjlj_x64_win.S` in
the archive (Windows only).

**3. The archive cannot simply gain `tur_rt_split.o`.** Adding it to
`libturt_runtime.a` is *not* inert: today's programs define those symbols
themselves, so any link that pulls the member for one symbol collides on the
rest. It needs its own archive (`libturt_preamble.a`), linked only in split
mode.

## Where it actually got to (2026-09-06)

`tur build --runtime=split` / `TUR_RUNTIME=split` is **wired and works for
simple and effectful programs, and is NOT correct for the suite.** Full run:
**2798 passed, 20 failed** against 1 failed in default mode. It is opt-in and
off by default, so nothing else is affected, but it must not be defaulted until
these are understood.

Landed and sound:

- generator emits the guarded `TUR_RT_SPLIT_HOSTED` forms (below), hash
  unchanged, JIT verified unaffected
- `libturt_preamble.a` -- its own archive, linked all-or-nothing
- `--runtime=split` + `TUR_RUNTIME=split`, reusing the JIT's own
  `jit_try_split_preamble` so the paths cannot drift, and declining LOUDLY
  (falling back to the full preamble) if the hash no longer matches
- the swap runs BEFORE `hoist_tur_include_directives`, because hoisting lands
  includes inside the preamble region and the swap was deleting them

### The 20, and what they say

| fixture class | symptom |
| --- | --- |
| `async-echo-server`, `async-file`, `async-timer-basic` | `no scheduler` |
| `fiber-local` | no output at all |
| `panic-catch-panic-of`, `panic-with-catch-of`, `catch-unwind-branch-result-return` | reaches a `should-not-reach` branch |
| `gc-registry-growth`, `schema-reader-json-*` | stdout mismatch |
| `hamt-lowering-basic` | build failed |

This is **not** a declarations problem -- `no scheduler` and a panic taking a
`should-not-reach` branch are runtime behaviour.

**Two hypotheses tested and RULED OUT**, so nobody re-runs them:

- *Duplicate state across the seam.* `nm`-diffing an async fixture's program
  object against `libturt_preamble.a` for defined data symbols returns
  **zero**. (The same diff on a trivial program is what found
  `tur_closure_headers_enabled`, so the method works.)
- *Two runtime vintages.* `tur_scheduler` appears in both
  `libturt_preamble.a` and `libturi.a`, which looks like the seam-3 hazard the
  S2 notes describe -- but `tur_rt_split.c` is a source of BOTH, so they are
  the same vintage, not a mix.

So the state is single-instance and single-vintage, and it is still not
initialized. That points at **initialization order or reachability**: something
the full preamble did on the way in is not happening when those definitions
arrive from an archive instead. `__tur_static_init` is the obvious suspect --
the generator deliberately keeps it `static` in the program half and drops it
from the runtime TU ("which neither defines nor calls it") -- so the next step
is to trace, on one async fixture, who is expected to create the scheduler and
whether that code runs at all under split.

Do that before touching the build wiring again; the wiring is not the problem.

## Implementation order

1. Generator: weak non-static global definitions in the impl half. Regenerate
   `src/runtime/generated/*`.
2. Confirm `tur_rt_split_hash` is **unchanged** -- it covers
   `emit_rt_split_source()`, the canonical preamble emission, not the generated
   artifacts, so a decls/impl change must not move it. Verify with
   `TUR_JIT_SPLIT_DEBUG=1` (prints probe vs committed) and re-run the JIT
   corpus. A silently-disengaged split is a documented failure mode:
   [jit-s2-split-disengages-on-hoisted-inline-c-include](../archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md).
3. New `libturt_preamble.a` from `tur_rt_split.c` + the sjlj asm.
4. `tur build` mode that swaps the preamble for the decls region and links the
   new archive. Keep `tur emit-c` emitting the full self-contained TU, so the
   ~2816 `expected.c` snapshots do not move and the user-facing artifact stays
   standalone-compilable.
5. Full suite under the new mode, then flip the default.

## Watch for

`hoist_tur_include_directives` (a JIT post-pass) injects `#include "hamt.h"`,
whose real prototypes conflict with the emitted loose `extern void
*tur_hamt_new();`. That is why a JIT dump does not drop straight into `cc` --
it is an artifact of that pass, not of the split, and the cc path does not have
it. Do not "fix" the loose prototypes chasing this.
