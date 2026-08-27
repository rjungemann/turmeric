# The compiler sanitizer gate exists, is verified, and nothing arms it

**Severity:** low, but it is the kind of low that becomes medium by attrition. A
gate nobody turns on decays to a gate nobody notices, which is how the bug it
was written for survived in the first place.

**Status:** RESOLVED 2026-08-26. The gate is armed in CI on both matrix legs.
Not a defect -- this was unfinished follow-through on
[fat-captures-borrowed-read-uninitialized](history/fat-captures-borrowed-read-uninitialized.md).

## Resolution

All three steps below are done. It took two passes on the same day, by two
different routes, and the split is worth recording because the second half was
the one nobody could do from a container:

**Pass 1 -- Linux (see "Progress" further down, kept as written).** Re-measured
the Linux zero at HEAD rather than trusting the 2026-08-25 number, then armed
that leg alone via `TUR_SANITIZER_GATE: ${{ runner.os == 'Linux' && '1' || '0' }}`
on the fixture-suite step. Also fixed `tests/run.sh` printing "They do not fail
the run" unconditionally -- including under `TUR_SANITIZER_GATE=1`, where the
run fails thirty lines later.

**Pass 2 -- macOS, which finishes it.** Measured the missing leg on an
Apple-silicon box (see "What was measured"), then dropped the `runner.os`
condition: the step now sets `TUR_SANITIZER_GATE: "1"` unconditionally. Kept at
step level, where pass 1 put it, rather than the job level -- that is the only
step that runs `tur_tests`, and `ctest` sets no `ENVIRONMENT` property on that
target, so the step's env reaches `tests/run.sh` unmodified.

Two supporting changes came with pass 2, without which an armed gate is not
actually actionable:

- `tests/run.sh` copies the findings log out of the `mktemp -d` results dir
  (which the `EXIT` trap deletes) to `$(dirname "$TUR")/sanitizer-findings.log`,
  overridable via `TUR_SANITIZER_LOG_OUT`. Before this, the `full log:` line the
  gate printed on failure named a path that no longer existed by the time
  anyone read it -- fatal for a gate whose entire job is to be readable from a
  CI log.
- `ci.yml` gained an `Upload compiler sanitizer findings` artifact step, so the
  per-fixture attribution survives a red run on a toolchain the reader does not
  have to hand. The console still prints only the top ten.

## What exists

`tests/run.sh` scans each phase's captured compiler stderr for
`: runtime error:` and reports UBSan/ASan findings from `tur` itself after the
summary. `TUR_SANITIZER_GATE=1` makes any finding fail the run.

It is verified in both directions on the full suite: with the
`fat_captures_borrowed` initializers deleted it reports 63 findings across 59
fixtures and exits 1 when armed; with them restored it reports nothing and exits
0 armed. The tree is currently at **zero findings**.

## What is missing

Nothing sets `TUR_SANITIZER_GATE=1`. `.github/workflows/ci.yml` runs the suite
via `ctest -R '^tur_tests$'` with no such variable, so CI is running the
unarmed configuration -- findings print into the log and the job stays green.

The default is unarmed on purpose (arming on discovery would have turned 60
silent findings into 60 red fixtures at once). That rationale expires once the
count is zero, and it is zero now.

## Why this is not just a flag flip

**The zero was measured on one platform.** Linux, GCC, this container's
toolchain. CI also runs macOS, where the compiler and libubsan differ, and
UBSan findings are exactly the class of thing that varies with toolchain --
different struct padding, different uninitialized bytes, different
implementation-defined corners. Arming it blind could redden the macOS leg on
findings nobody has looked at.

So the work is:

1. Run the suite on each CI platform with the gate armed and record the count.
   The Linux number is 0; macOS is unmeasured.
2. Fix or triage whatever the other platforms report. A finding that only
   reproduces on one toolchain is still a real finding -- it was just never
   visible before.
3. Then set `TUR_SANITIZER_GATE: 1` in the workflow env for the jobs that run
   `tur_tests`.

Step 1 is the whole risk, and it cannot be done from a Linux container.

## What was measured

Step 1 was carried out on an Apple-silicon host, which is what the report was
waiting for.

| leg | toolchain | fixtures | findings |
| --- | --- | --- | --- |
| Linux | GCC, container | -- | 0 (previously measured) |
| macOS | Apple clang 21.0.0, arm64, macOS 27.0 | 2703 | **0** |

**One honest gap:** the macOS row was measured on a local Apple-silicon Mac,
not on the `macos-latest` runner image, whose macOS and Xcode versions differ.
Same libubsan lineage, same architecture, so it is strong evidence rather than
proof, and it is a far better basis than the Linux-only zero the gate would
otherwise have been armed on. The residual risk is one CI run wide: if the
runner's clang reports something this host does not, the macOS leg goes red
once with the finding attributed in the uploaded artifact. That is the
intended failure mode, not a surprise.

`TUR_SANITIZER_GATE=1 bash tests/run.sh` exited 0 with `summary: 2703 passed,
0 failed` and no `SANITIZER:` block at all. Step 2 was therefore a no-op: there
was nothing to triage.

**The zero was verified to be a real zero, not a broken detector.** A gate that
reports nothing because its pattern does not match the local sanitizer's output
looks exactly like a clean tree, and this one had only ever been exercised
against GNU libubsan. Two checks:

- Apple clang's UBSan emits the same `<file>:<line>:<col>: runtime error: ...`
  shape GNU does, so the harness's `: runtime error:` grep matches unchanged.
- End-to-end positive control, mirroring how the Linux zero was verified: a
  signed overflow planted in `main()`, rebuilt, one fixture run armed. It was
  caught, attributed to its fixture and phase, written to the durable log, and
  failed the run --

  ```
  SANITIZER: 1 finding(s) from `tur` across 1 fixture(s).
    TUR_SANITIZER_GATE=1: these are FATAL; the run fails below.
         1 build  src/main.c:9712:51: runtime error: signed integer overflow: ...
  FAILED: 1 sanitizer finding(s) from the compiler (TUR_SANITIZER_GATE=1).
  ```

  The control was then reverted and `tur` rebuilt.

One thing to know for the next person: the `-DTUR_DEBUG_SANITIZE=OFF` escape
hatch in CLAUDE.md, and the Homebrew-LLVM workaround next to it, both exist for
an ASan startup deadlock caused by an outdated Apple toolchain. Neither is
needed here -- Apple clang 21 on darwin 27 runs the sanitized build fine -- and
either one would silently produce a meaningless zero for this measurement,
since the first strips the sanitizers outright and the second measures a
toolchain CI does not use. Measure with the stock `cc` the macOS runner has.

## Do not do instead

Turning on `-fno-sanitize-recover` in the Debug build. That makes UBSan abort
rather than print, which sounds equivalent and is not: it converts every finding
into a hard crash mid-compile, so the suite reports "tur build failed" with no
diagnostic instead of a labelled sanitizer line, and one finding takes down
every fixture that reaches it. The grep-based gate reports *all* findings in a
run and attributes each to a fixture and a phase; that is strictly more useful
for the triage step above.

---

## Progress (2026-08-26): Linux armed, macOS still open

Step 1 was re-measured on Linux and step 3 done for that leg only. What is left
is steps 1-3 **on macOS**, which still needs a mac.

### Linux is armed

`.github/workflows/ci.yml`'s "Run fixture suite" step now carries:

```yaml
env:
  TUR_SANITIZER_GATE: ${{ runner.os == 'Linux' && '1' || '0' }}
```

The condition is the report's own reasoning, kept in the workflow so the next
reader does not have to find this file to know why the two legs differ: the
zero was measured on Linux/GCC, UBSan findings vary by toolchain, and arming
macOS blind would redden that leg on findings nobody has looked at. The step
comment says exactly what finishing looks like -- measure on a mac, triage,
drop the condition.

`ctest` sets no `ENVIRONMENT` property on `tur_tests`, so the step's env reaches
`tests/run.sh` unmodified.

### Re-measured, not taken on faith

The report's Linux zero was from 2026-08-25. Re-run here at HEAD with the gate
armed: still zero findings, suite green. Worth re-taking rather than trusting
the number, since several compiler changes have landed since.

### One thing fixed on the way

`tests/run.sh`'s findings block printed

```
  They do not fail the run (set TUR_SANITIZER_GATE=1 to make them fatal).
```

unconditionally -- including under `TUR_SANITIZER_GATE=1`, where the run fails
thirty lines later. A CI log that says a finding is harmless and then fails the
job on it is worse than either message alone. The line is now conditional and
says which configuration is running.

### Why arming one leg is not half-measures

The remaining risk was always macOS; Linux carried none. Leaving Linux unarmed
until a mac is available means the platform where the count *is* known keeps
running the configuration that let the original bug hide for as long as it did.
The two legs are independent -- a Linux regression in this class now fails CI,
whatever macOS is doing.

### What is still needed (unchanged from above)

*(Done in pass 2 -- see "Resolution" and "What was measured" at the top. Left
as written because the reasoning in the last paragraph still stands: a future
platform added to the matrix gets measured before it gets armed.)*

1. Run the suite on macOS with `TUR_SANITIZER_GATE=1` and record the count.
2. Fix or triage whatever it reports.
3. Drop the `runner.os` condition from the workflow env.

Do NOT instead flip the whole matrix and see what happens: that is the
"60 red fixtures at once" failure mode the default-unarmed design exists to
avoid, just relocated to a platform nobody has looked at.
