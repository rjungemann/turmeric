# `#lang r7rs`: the conformance program emits 5.6 MB of C and takes ~4 minutes to build

**Narrowed 2026-10-03: fix direction 1 landed.**  The direct->cps entry
wrapper of a zero-parameter colored function that is not a T6 bouncer is now
a two-line shim over one shared helper, `__dk_enter0` (emitted once per unit
beside `tur_async_suspended`, emit_module.c), which runs exactly the inline
wrapper's sequence -- root prompt, trampoline driver, body, a boxed result
copied out before the reap, reap.  In the conformance program 1,559 of the
entries take it.  Measured on one idle 4-core box, against the same program
unit with the shims expanded back to the old inline bodies:

| | before | after |
| --- | --- | --- |
| program unit | 5.66 MB | 4.78 MB |
| `cc -O2` on it | 90.5 s | 35.1 s |

So the per-function `setjmp` copies were most of the optimizer's time, not
just ~1 MB of text.  Still open: the `emit-c` phase itself (130 s under the
Debug `tur` here), and directions 2-3 (`main` and `__tur_fatbox_init` as
single huge functions), which matter less now that cc is a third of what it
was.  The harness's `--timeout 480` workaround stays until CI shows the new
wall clock.

**Filed 2026-10-01**, investigating CI #3079.

**Severity:** medium (CI wall-clock and stability, and build cost for any large
`#lang r7rs` program). Nothing is miscompiled. The program builds and passes.
The cost is the time it takes.

## Summary

The compiled half of `tur_r7rs_conformance` builds chibi's whole R7RS suite as
one program. That is 94 KB of Scheme in 1,181 top-level forms, and the
program's own C unit comes out at **5.6 MB** (98,788 lines), about 60 times
the source. The cached prelude is a separate unit and is not counted here.
Under the sanitized Debug `tur` that CI runs, the build takes **248 s** on an
idle 4-core box:

| phase | time |
| --- | --- |
| `tur emit-c` (Debug, ASan+UBSan) | 95 s |
| `cc -O2` on the program unit (one process, one thread) | ~150 s |
| prelude library unit | cached; not part of this cost |

The harness, `tests/r7rs/run-conformance.py`, gave each program run 240 s by
default, and its runner's comment still read "one C build (a minute)". The
budget stopped holding once PR #977 (`88e1ab44`) took `RUN_SERIAL` off the
test, so it shares the runner's cores with the other r7rs suites.
CI's Linux timings from the `ci-metrics` branch, for the test as a whole:

| | fast runner (`tur_r7rs_gc` ~240-305 s) | slow runner (`tur_r7rs_gc` ~380-425 s) |
| --- | --- | --- |
| before #977 (`RUN_SERIAL`) | 83 s | 145 s |
| after #977 | 118-185 s | 187-247 s |

Three `main` runs failed at the cap: `82ccdc55`, `7310b0c5` and `efa2258c`
(CI #3052, #3054, #3079). PR runs #3070 and #3078 failed the same way. Each
showed `[compiled] round 1: timeout -- 1181 form(s) not run`, scored 0 passed
against a floor of 1223, and took ~246 s. One slow-runner run that passed
(`5fd23a65`) cleared the cap by about 3 s.

**Worked around** in the same change that filed this report.
`tests/run-r7rs-conformance.sh` now passes `--timeout 480`
(`R7RS_CONFORMANCE_TIMEOUT` overrides it), and ctest's `TIMEOUT 720` still
bounds the whole test. That makes room for the cost without reducing it.
This report is about the cost.

## Repro

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
# Write out the exact program the compiled round builds:
python3 - <<'EOF'
import importlib.util
spec = importlib.util.spec_from_file_location("rc", "tests/r7rs/run-conformance.py")
rc = importlib.util.module_from_spec(spec); spec.loader.exec_module(rc)
forms = [f for f in rc.split_forms(open(rc.SUITE).read()) if rc.form_head(f[2]) != "import"]
open("/tmp/conf.tur", "w").write(rc.build_program(forms, range(len(forms)))[0])
EOF
time ASAN_OPTIONS=detect_leaks=0 ./build/tur emit-c /tmp/conf.tur > /tmp/conf.c   # ~95 s
time ASAN_OPTIONS=detect_leaks=0 TUR_SHOW_CC=1 ./build/tur build /tmp/conf.tur -o /tmp/conf   # ~248 s
ls -l /tmp/tur-build/_tmp_conf_tur.c    # the program unit cc compiles, ~5.6 MB
```

`R7RS_CONFORMANCE_BACKEND=compiled R7RS_CONFORMANCE_TIMEOUT=240 bash
tests/run-r7rs-conformance.sh` reproduces the CI failure on such a box.

## Where the 5.6 MB goes

These figures are from counting function bodies in the program unit.
Measured, not yet explained:

| what | functions | bytes |
| --- | --- | --- |
| `__fn_N` lambdas | 3,797 | 2.59 MB |
| `__fn_N__cps` (CPS bodies of those lambdas) | 1,563 | 0.60 MB |
| `main` (all 1,181 top-level forms, one function, 4,689 lines) | 1 | 0.42 MB |
| `__tur_fatbox_init` (3,420 lines of registration calls) | 1 | 0.27 MB |
| `tur_`/`r7rs_`/`r7bn_`-prefixed helpers written into the program unit | 489 | 0.27 MB |
| other named functions | 157 | 0.11 MB |
| named `*_cps` | 51 | 0.07 MB |
| `drop_glue_*` | 410 | 0.05 MB |
| outside function bodies (prototypes, ~10,800 lines; types) | -- | 1.26 MB |

The lambdas are expected in number. The harness's `test` macro wraps every
test in a `(lambda () expr)` thunk, and the expressions bring their own. Their
**size** is what stands out. The median lambda is 730 bytes, and for a
CPS-colored one that whole body is the direct-style entry shim. Here is one
verbatim:

```c
__attribute__((unused)) static tur_tagged_t __fn_1002() {
    tur_sl___dk_entry_depth++;
    size_t __dk_reap_mark = tur_sl___dk_reap_n;
    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());
    int64_t __r;
    tur_jmp_buf __dkjb; tur_jmp_buf *__dksave = tur_sl_g_dk_driver; tur_sl_g_dk_driver = &__dkjb;
    if (TUR_SETJMP(__dkjb) == 0) { __r = __fn_1002__cps(__root); }
    else { __r = __dk_drive_after(); }
    tur_sl_g_dk_driver = __dksave;
    tur_tagged_t __ret = __r ? (*(tur_tagged_t *)(__r)) : (tur_tagged_t){0};
    if (!tur_sl_tur_async_suspended) dk_free(__root);
    if (!tur_sl_tur_async_suspended) { if (--tur_sl___dk_entry_depth == 0) __dk_reap_run(); else __dk_reap_drop_to(__dk_reap_mark); }
    return __ret;
}
```

Every one of the 1,563 colored lambdas has a shim like this beside its
`__cps` body. With the function's own name replaced, 1,557 of them are
byte-identical (four shapes in all). Each also contains a `setjmp` that cc
has to treat conservatively.

## Not measured yet

- **What cc spends its 150 s on.** The single 4,689-line `main` and the
  3,420-line `__tur_fatbox_init` are the usual suspects for superlinear
  optimizer passes. `gcc -O2 -ftime-report` on the saved unit would say.
- **How much of the 95 s `emit-c` is the sanitizers.** A Release `tur` was
  not timed.
- **Why 1,563 of the lambdas are CPS-colored.** The program imports
  `(scheme eval)`, every test runs inside the harness's `guard`, and the
  suite calls `call/cc` (6 sites) and `dynamic-wind` (1), so wide coloring
  is plausible. Nothing here checks that it is necessary.

## Fix directions

1. **One shared entry shim.** Emit the direct-style wrapper as one helper,
   or a macro around one helper, that takes the CPS function pointer, so each
   colored lambda does not get its own copy. Every CPS program benefits. This
   one alone removes most of 1.0 MB here (1,557 identical shims of ~670
   bytes).
2. **Split `main`.** Emit the top-level forms in chunks of N forms per
   `static` function, so cc never sees a 4,689-line function. This matters
   if `-ftime-report` shows `main` dominating.
3. **Make `__tur_fatbox_init` table-driven.** Use a static array of
   (key, arity) rows and one loop, the way the archived
   `r7rs-programs-compile-slowly` fix made the fat-box table address
   constants.
4. **Harness side, if the above are slow to land:** build the conformance
   program at a lower `-O`, or split the compiled round into a few programs
   so they build in parallel. The first changes what the suite tests, and
   the second changes how a crash is attributed. Both would be the suite
   owner's call.

`tur_r7rs_srfi_suites_*` uses the same harness and still runs on the
default 240 s per program. Its programs are much smaller (a whole shard
takes 157-261 s for every SRFI together), so it is not at risk today. If one
SRFI's suite grows to this size, it will hit the same cap.
