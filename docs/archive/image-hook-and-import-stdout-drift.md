# Four pre-existing stdout mismatches: image hooks + cross-module import init

> **RESOLVED (2026-06-28).** All four fixtures
> (`image-hooks-tracked`, `image-reload-hook`, `image-roundtrip`,
> `load-in-imported-module`) now PASS on the default by-value gate
> (`bash tests/run.sh` -> 1870 passed, 0 failed). The fix is genuine, not a
> masked snapshot: each `expected.stdout` still encodes the CORRECT behavior
> (e.g. `image-roundtrip` expects `init ran` only at cold-load, never on warm
> reload) and actual output matches it -- the init hook no longer double-fires
> on warm resume, and cross-module `(load ...)` init order is correct. Verified
> deterministic across repeated fresh runs. Archived per the strict
> resolved-report rule. See `docs/archive/history/` for the verification trail.

**Severity:** low -- four fixture-level regressions in the gate suite
that do not block compilation or affect the desktop-editor track.

These four failures persisted after Option A (hoisting GCC nested
functions out of `stdlib/future.tur` and `stdlib/taskgroup.tur`) and
after the Apple clang 17 workarounds in
[`clang17-wint-conversion-codegen.md`](clang17-wint-conversion-codegen.md).
Confirmed pre-existing by stashing the Option A changes and re-running
the same fixtures -- both regressions reproduce against HEAD.

## Affected fixtures

| Fixture | Family | Symptom |
| --- | --- | --- |
| `image-hooks-tracked`   | image / persistence | stdout mismatch |
| `image-reload-hook`     | image / persistence | stdout mismatch |
| `image-roundtrip`       | image / persistence | stdout mismatch |
| `load-in-imported-module` | module loading / import | stdout mismatch |

## Repro

```sh
bash tests/run.sh 2>&1 | grep "^FAIL"
```

Sample diff (`image-roundtrip`):

```
--- tests/fixtures/image-roundtrip/expected.stdout    2026-06-10
+++ tests/fixtures/image-roundtrip/actual.stdout      2026-06-26
 init ran
 loop ran
 warm:
+init ran
```

`init ran` is being printed once at warm-load when the expected
output has it only at cold-load. The fixture exercises Turmeric's
image-persistence story (snapshot a runtime, reload it, expect
init-hook to NOT fire again on reload). The actual behavior is firing
the hook twice.

The three other image-family fixtures share root structure; symptoms
diverge in detail but the family hangs together.

`load-in-imported-module` is a separate category -- looks like an
init-order issue in cross-module `(load ...)` rather than image
snapshotting.

## Why ship the report instead of the fix

- The desktop-editor track is the active priority. These four
  failures don't block any of its phases (the plugin doesn't touch
  image hooks or cross-module init order).
- The fix requires understanding `runtime/image.c`'s init-hook
  registry plus the module-graph elaboration for cold vs. warm
  resume, which is a non-trivial dive.
- Test drift here is presumably not caused by Apple clang 17 (image
  hooks don't use any of the C constructs clang 17 newly rejected),
  so this would have failed on the previous compiler too if it had
  ever been run there. Likely a recent semantic regression in the
  image / load pipeline that the suite was masking until I fixed the
  larger build-failure cluster.

## Suggested follow-up

1. Bisect on `git log -- runtime/image.c stdlib/image.tur` for the
   commit that broke `image-roundtrip` -- the symptom (init hook
   firing on both cold and warm load) is narrow enough to localize.
2. Separately bisect on `compiler/elab_module.c` / `compiler/elab_global.c`
   / `runtime/runtime.c` for `load-in-imported-module`.
3. Once a commit is identified, decide whether the right fix is to
   revert (if it was an unintentional regression) or to update the
   expected stdouts (if the new behavior is the desired one and the
   fixtures were never updated).
