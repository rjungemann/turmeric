# `serial-shift` in an unsupported context silently miscompiles

**One-line summary.** A `serial-reset`/`serial-shift` whose delimited context
falls outside the DK-lowering grammar (`collect_ctx`) is silently lowered to a
placeholder that returns `0` without calling the receiver -- a **silent
miscompile**, not a diagnostic. With `save-cont!`/`resume-cont!` now wired to
the real serial runtime, the same unsupported shapes additionally produce a
**broken binary** (implicit declaration of `tur_serial_cont_*`).

**Severity.** High. Two failure modes, both silent at compile time:
1. Wrong result, no error (the receiver `k` is never invoked; the shift
   evaluates to `0`).
2. Miscompiled binary that crashes at runtime (`Illegal instruction`) when the
   serial runtime prelude is not emitted but `save-cont!`/`resume-cont!` are.

---

## Repro

Build: `./build/tur build <file> -I stdlib -o /tmp/x && /tmp/x`

### A. Silent wrong result (1-arg call context)

```turmeric
(load "stdlib/workflow.tur")
(defn dbl [x : int] : int (* x 2))
(defn rt [k : ptr<void>] : int (resume-cont! (save-cont! k) 5))
(defn main [] : int
  (println (serial-reset (dbl (serial-shift rt 0))))   ; expected dbl(5) = 10
  0)
```

- **Observed:** prints `0`.
- **Expected:** `10` -- or, failing that, a *compile error* saying the context
  is not capturable. Instead the shift is silently discarded
  (`emit_effects_serial_shift` emits `int64_t t = 0`), `dbl(0) = 0`.

`dbl` is a **1-arg** call; `collect_ctx` only accepts **2-arg** calls
(`src/compiler/emit_cps.c:690`, `cur->as.call_.n_args == 2`), arithmetic
binops, scalar `let` preludes, and one `if`. Anything else -> `sk_can_lower`
returns false -> `emit_cps_serial_reset` returns NULL ->
`emit_effects_serial_shift` placeholder.

### B. Broken binary (do-sequence context)

```turmeric
(load "stdlib/workflow.tur")
(defn rt [k : ptr<void>] : int (resume-cont! (save-cont! k) 0))
(defn work [] : int (println "in main-loop") 42)
(defn main [] : int
  (println (serial-reset (do (serial-shift rt 0) (work))))
  0)
```

- **Original observation:** `Illegal instruction`, with compiler warnings
  `implicit declaration of function 'tur_serial_cont_deserialize'` /
  `'tur_serial_cont_resume'`.
- **Root cause (dangling builtin -- FIXED this session):** the serial runtime
  prelude (`tur_serial_cont_serialize/_deserialize/_resume`) was emitted only
  when `emit_cps_program_uses_serial_dk(program)` was true, i.e. when some
  `serial-reset` *can lower*. A `do` sequence is not in the grammar, so the
  gate was false and the prelude absent -- but `resume-cont!` (now wired,
  `stdlib/workflow.tur`) still references the builtins. **Fix:** broadened the
  gate to `emit_cps_program_contains_serial` (presence of any serial node, not
  just lowerable resets) at `src/compiler/emit_module.c:2916,2927`. The prelude
  is now always present when serial syntax is used, so the references resolve.
- **Remaining (pre-existing) crash:** with the prelude present, Test B *still*
  `SIGILL`s -- because a `serial-shift`/`serial-reset` in **statement
  position** (its value discarded, e.g. the non-final item of a `do`) lowers to
  `__builtin_trap()` (`src/compiler/emit_stmt.c:360-363`). This mirrors the
  identical `cloneable` trap two cases above and predates this work. It is a
  blunt "unsupported" marker (a trap, not a silent wrong answer), but a bare
  `SIGILL` with no diagnostic is poor ergonomics -- it should be a named
  `TUR-E00xx` error. Tracked here as the statement-position facet of the same
  underlying gap.

---

## Root cause

- Supported grammar: `collect_ctx` (`src/compiler/emit_cps.c:634-742`) accepts
  a single-scalar-hole chain of {scalar `let` prelude, `+ - * /` binop, 2-arg
  top-level call, one `if`} bottoming out at the shift. This is a
  proof-of-concept context serializer, documented as "fall back to legacy for
  unsupported shapes" in `docs/archive/history/cps-transform-plan.md:515`.
- For the base `shift`/`reset`, "legacy" is a real snapshot lowering. For
  `serial-shift` there is **no** legacy implementation -- the fallback
  (`emit_effects_serial_shift`, `src/compiler/emit_effects.c:1528`) is a stub
  that emits `0`. So "fall back to legacy" silently means "return 0".
- Prelude gating (`emit_module.c:2916-2929`) keys off "a serial-reset that can
  lower", not "the serial builtins / `save-cont!` / `resume-cont!` are
  referenced", so the wired stdlib functions can dangle.

## Proposed fix directions

1. **Make the unsupported case a hard error.** When a `serial-reset` contains a
   `serial-shift` but `sk_can_lower` is false, emit a real diagnostic
   (`TUR-E00xx: serial-shift context is not capturable`) instead of the silent
   `0` placeholder. This closes both A and B (B never reaches the dangling
   builtin ref because compilation stops). Lowest-risk, most honest.
2. **Broaden the prelude gate** so the serial runtime (and its DK-machine
   dependency) is emitted whenever a `serial-shift`/`serial-reset` node exists
   *or* the `tur_serial_cont_*` builtins are referenced -- not only when a
   reset lowers. This removes the crash in B but leaves A's silent-`0` result.
   Pairs well with (1).
3. **Generalize the capture grammar** (large): a real CPS-based continuation
   serializer that handles `do` sequences, n-ary calls, and arbitrary control,
   so contexts like B actually capture. This is what
   `docs/upcoming/application-image-dumps-plan.md` (AI1-AI8) needs and is a
   major compiler effort, not a wiring task.

## Validation

- A/B above must either produce the correct result (after grammar
  generalization) or fail with a named compile error -- never a silent `0` or
  an `Illegal instruction`.
- Supported contexts (`tests/fixtures/serial-context-marshal`, and the
  `save-cont!`/`resume-cont!` round-trip added in this session) must keep
  passing.
