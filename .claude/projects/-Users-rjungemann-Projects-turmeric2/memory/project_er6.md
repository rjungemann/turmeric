---
name: ER6 Effects Phase Status
description: Implementation status of ER6 effect row polish/ergonomics phase
type: project
---

ER6 is complete as of 2026-05-17. All compiler implementation was already done; the session added fixtures and stdlib annotations.

**What was done in this session:**
- Created fixtures: `effect-strict-mode`, `effect-row-compose`, `effect-subtype-capability`, `errors/effect-double-resume-static`, `stdlib-effects-annotated`
- Added `#{Unsafe}` annotation to `read-int-console` in `stdlib/effects.tur`
- Annotated all defns in `stdlib/vec.tur`, `stdlib/log.tur`, `stdlib/thread.tur`, `stdlib/async_file.tur`, `stdlib/async_socket.tur`, `stdlib/async_pipe.tur` with `#{Unsafe}`
- Added `check-mode-effect-error` and `check-mode-effect-ok` flag tests in `run-flags.sh`

**What is IMPLEMENTED but not yet tested (compiler):**
- TUR-W0032 (row variable always concrete) -- diag code exists in diag.h but no emission in effect_check.c
- Typeclass default method effect row checking -- TypeClassMethod has no default_body field

**Phase status: 680 fixture tests + 37 flag tests pass, 0 failures.**

**Why:** ER6 is a prerequisite for ET0-ET4 (effect types), which are prerequisites for LC0-LC3 (linear continuations) and MS0-MS4 (multi-shot).
**How to apply:** When starting ET0, verify ER6 is fully stable first.
