# The compiler sanitizer gate exists, is verified, and nothing arms it

**Severity:** low, but it is the kind of low that becomes medium by attrition. A
gate nobody turns on decays to a gate nobody notices, which is how the bug it
was written for survived in the first place.

**Status:** OPEN. Not a defect -- unfinished follow-through on
[fat-captures-borrowed-read-uninitialized](../archive/history/fat-captures-borrowed-read-uninitialized.md).

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

## Do not do instead

Turning on `-fno-sanitize-recover` in the Debug build. That makes UBSan abort
rather than print, which sounds equivalent and is not: it converts every finding
into a hard crash mid-compile, so the suite reports "tur build failed" with no
diagnostic instead of a labelled sanitizer line, and one finding takes down
every fixture that reaches it. The grep-based gate reports *all* findings in a
run and attributes each to a fixture and a phase; that is strictly more useful
for the triage step above.
