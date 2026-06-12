# turi: serial-shift / cloneable-shift (context-capturing delimited control) unimplemented

> **RESOLVED (2026-06-12): implemented via runtime context reification.** The
> interpreter now evaluates the context-capturing `serial-shift` /
> `cloneable-shift`.  Rather than a fiber or a compile-time DK walk, `EX_SERIAL_RESET`
> / `EX_CLONEABLE_RESET` reify the delimited context **at runtime**
> (`ts_capture_and_run`, `src/turi/eval.c`): they walk the reset body down the
> unique shift-reaching child through the same grammar `collect_ctx` accepts --
> single-hole int `+ - * /` binops, 1- and 2-arg top-level call frames, a pure
> `let`, an `if` with one shift-bearing arm, and a `do`-sequence prelude +
> ignore-value tail -- evaluating each non-hole operand once at capture time and
> recording it as a frame.  The captured continuation is that frame array, boxed
> as an int64 handle; resuming folds the frames innermost-first, so it is
> multi-shot for cloneable and (in-process) marshalable for serial.  The resume /
> clone / serialize / deserialize builtins (`tur_{cloneable,serial}_cont_*`) and
> `stdlib/workflow.tur`'s `save-cont!` / `resume-cont!` are wired as interpreter
> natives over this machinery.  An uncapturable context (a shape outside the
> grammar) now raises the compiled path's `TUR-E0706` under `--interpret` instead
> of the old silent / empty-stderr failure -- closing the negative path too.
>
> **13 of 14 context-capturing fixtures pass under `--interpret`** and are on the
> `run-turi.sh` TI3 allowlist: `serial-context-{marshal,let,if,if-outer-frames,call1,do,do-cfg}`,
> `cloneable-context-{multishot,let,if,if-outer-frames}`, `context-call-frame`,
> `context-division`; plus the 2 `errors/serial-context-{,do-}not-capturable`
> negative fixtures (TUR-E0706), which were removed from `TURI_ERRORS_DENY`
> (that denylist is now empty).  `EX_SERIAL_SHIFT` / `EX_CLONEABLE_SHIFT` were
> removed from `docs/turi-carve-out.txt`; the parity ratchet passes (111/115
> handled, 4 carved).  Full turi harness 967 passed / 0 failed.
>
> **One carve-out remains:** `serial-context-do-struct` (marked
> `requires.compiled`) -- its struct env routes through a `Serializable` instance
> over inline-C struct accessors (`malloc`+field stores, `p[0]+p[1]`) the
> tree-walking interpreter cannot execute.  That is the inline-C-evaluator gap
> ([turi-inline-c-silent-miscompiles.md](turi-inline-c-silent-miscompiles.md)),
> not the capturing-shift substrate.  `call/cc*` (`EX_CALLCC`) stays a separate
> carve-out per the CPS-transform plan.  The original analysis is kept below.

**Summary:** The tree-walking interpreter (`turi`) implements the *abortive*
delimited-control operators -- `reset`, `shift`, `shift0`, and the
no-shift case of `serial-reset` / `cloneable-reset` (Phase TI3) -- but does
**not** yet implement the *context-capturing* variants `serial-shift` and
`cloneable-shift`, which hand a resumable continuation to their receiver. A
program that performs `serial-shift` or `cloneable-shift` runs correctly under
`tur build` / `tur run` (compiled) but errors under `tur --interpret`.

**Severity:** Ergonomics / parity gap (not a miscompile). The interpreter
errors out cleanly; it never produces a wrong answer. Tracked as the remaining
slice of TI3 in
[docs/upcoming/v1/turi-parity-post-v1-plan.md](../upcoming/v1/turi-parity-post-v1-plan.md).

> **Update (2026-06-12): corrected substrate analysis -- the original
> "one-shot serial via a fiber" framing below is wrong.** A read of the actual
> implementation (`src/compiler/emit_cps.c`, `src/runtime/cps_prompt.c`) and of
> every `serial-context-*` / `cloneable-context-*` fixture shows that serial and
> cloneable continuations are **not** native-stack fiber snapshots that could be
> suspended/resumed with the `TuriEffectCont` machinery. They are
> **heap-reified DK chains** built by a *compile-time*, grammar-restricted walk
> of the enclosing reset body (`collect_ctx`, `emit_cps.c:638`), where each
> context frame is emitted as a generated C function `intptr_t (*)(intptr_t env,
> intptr_t value)` carrying only an int/cstr env (`DKFrame`, `cps_prompt.h:34`;
> `env_kind_ok` restricts the env to `TY_INT`/`TY_CSTR`). Consequences that
> invalidate the fiber plan:
>
> - **Serial is already multi-shot.** `serial-context-do`'s `twice` saves one
>   continuation and resumes it *twice* (`(resume-cont! b 0)` twice); `dk_invoke`
>   supports this by `dk_copy`-ing the chain per resume (`cps_prompt.c:161-164`).
>   A one-shot fiber cannot pass it.
> - **Every serial fixture marshals or resumes via natives.** All of them call
>   `tur_serial_cont_resume`, and most round-trip through
>   `tur_serial_cont_serialize` / `_deserialize` (or the `save-cont!` /
>   `resume-cont!` wrappers in `stdlib/workflow.tur`). A native C-stack snapshot
>   is by definition **not** serializable -- that is the entire point of the
>   "serial" (serializable) flavor. So a fiber substrate can pass *none* of the
>   serial-context fixtures.
> - **Serial and cloneable are not "easy one-shot" + "hard multi-shot."** Both
>   ride the *same* `collect_ctx` + DK chain (`emit_cps.c` calls it with
>   `EX_SERIAL_SHIFT` and `EX_CLONEABLE_SHIFT`). The only real difference is
>   marshaling: cloneable = in-process multi-shot (no serialize), serial = adds
>   serialize/deserialize. Neither is a fiber.
>
> The corrected root cause and fix directions are below; the original text is
> kept (struck) for history.

## Background: two flavors of shift in Turmeric

The `EX_SHIFT` / `EX_SHIFT0` nodes are **abortive**. The compiled path lowers
`(shift f body)` to: evaluate `body` to `v`, compute `f(v)`, then abort the
computation up to the nearest enclosing `reset`, whose value becomes `f(v)`.
The captured sub-continuation is never resumed -- the emitted runtime body is
`__dk_abort_body`, which ignores the captured slice (see the generated C and
`src/runtime/cps_prompt.c`). `shift0` differs only in prompt re-installation
on resume; since the continuation is discarded, it behaves identically.

Because the continuation is discarded, a plain `setjmp`/`longjmp` prompt
boundary models these exactly -- this is what TI3 shipped in `src/turi/eval.c`
(`eval_reset_boundary` / `eval_abortive_shift`).

By contrast, `serial-shift` and `cloneable-shift` pass a **resumable
continuation** `k` to their receiver `f`. `f` may invoke `(k w)` to resume the
delimited context (the frames between the shift and its reset) with `w`:

```turmeric
;; serial-context-marshal: (+ 10 []) is captured; (k 5) resumes it -> 15
(serial-reset (+ 10 (serial-shift (fn [k] (k 5)) 0)))   ; => 15
```

`cloneable-shift` additionally supports **multi-shot** resume -- invoking `k`
more than once, each from the same capture point:

```turmeric
;; cloneable-context-multishot: (10+1) + (10+2) = 23
(cloneable-reset (+ 10 (cloneable-shift (fn [k] (+ (k 1) (k 2))) 0)))  ; => 23
```

## Observed vs. expected

Minimal repro (`/tmp/cap.tur`):

```turmeric
(defn main [] : int
  (println (serial-reset (+ 10 (serial-shift (fn [k] (k 5)) 0))))
  0)
```

- `tur run /tmp/cap.tur`        -> prints `15` (compiled path).
- `tur --interpret /tmp/cap.tur` -> exits non-zero; `serial-shift` hits the
  default arm in `src/turi/eval.c` ("unhandled expression kind ...").

Expected: parity -- the interpreter prints `15` as well.

## Root cause (corrected)

`src/turi/eval.c` has no `case EX_SERIAL_SHIFT:` / `case EX_CLONEABLE_SHIFT:`
arm; they fall through to the default "unhandled expression kind" error. But the
gap is deeper than a missing arm, because the **continuation is captured at
compile time**, and the interpreter never runs the compile phase that does it:

1. **Capture is a compile-time context walk, not a runtime stack snapshot.**
   `collect_ctx` (`src/compiler/emit_cps.c:638`) statically walks the body of
   the enclosing `serial-reset` / `cloneable-reset` and recognizes a restricted
   grammar -- arithmetic frames, 1- and 2-arg call frames, a pure `let`, an `if`
   with exactly one shift-bearing arm, and a `do`-sequence with a
   statement-position shift -- reifying the delimited context (the part between
   the reset and the shift) into a **DK chain** (`src/runtime/cps_prompt.c`).
   Each frame is emitted as a generated C function
   `intptr_t (*)(intptr_t env, intptr_t value)` (`DKFrame`, `cps_prompt.h:34`)
   whose env is a single int/cstr (`env_kind_ok`). The tree-walking interpreter
   has none of this: at a shift site it is mid-`eval_expr` recursion with the
   context held implicitly in the **C call stack**, with no reified frames to
   hand to `f`.

2. **Resume / marshal are runtime natives over the DK chain.** The handle `f`
   receives is a DK-chain pointer; `tur_serial_cont_resume(k, v)` runs it
   (`dk_invoke`, which `dk_copy`s for multi-shot), and
   `tur_serial_cont_serialize` / `_deserialize` marshal it to/from a
   length-prefixed buffer (the static prelude emitted at `emit_cps.c:1752+`).
   These are builtins (`builtins.c:210-212`), **not** registered as interpreter
   natives, so even with a DK chain in hand the interpreter cannot resume it.
   `stdlib/workflow.tur`'s `save-cont!` / `resume-cont!` are thin wrappers over
   the serialize/resume builtins, so the `serial-context-call1` / `-do` fixtures
   need the same natives.

3. **`TUR-E0706` ("context not capturable") fires in codegen, which the
   interpreter never reaches.** The capturability check is emitted in
   `src/compiler/emit_effects.c:1537` and `emit_stmt.c:375` -- the **emit
   phase**, downstream of elaboration. The interpreter stops at elaboration, so
   it never runs the check; that is exactly why the two negative fixtures
   `serial-context-not-capturable` / `serial-context-do-not-capturable` exit 1
   with **empty stderr** under `--interpret` instead of emitting `TUR-E0706`
   (the TI3.2 carve-out also tracked in
   [turi-error-fixture-diag-divergences.md](turi-error-fixture-diag-divergences.md)).

## Proposed fix directions (corrected)

There is no cheap fiber-backed one-shot slice. Both flavors need the same
reified-context substrate; the realistic options are:

- **(A) Reify the delimited context as data in the interpreter.** Either run a
  `collect_ctx`-equivalent over the enclosing reset body *at runtime* to build a
  DK chain whose frames invoke turi closures (an int/cstr-env subset, matching
  the grammar), or restructure `*-reset` evaluation into an explicit-continuation
  form so the context is reified rather than living on the C stack. Then provide
  turi-aware DK `run` / `copy` / `serialize` / `deserialize` and register
  `tur_serial_cont_resume` / `_serialize` / `_deserialize` as interpreter
  natives. This is a self-contained sub-project, comparable in size to the
  compiled CPS lowering -- not a single `case` arm.
- **(B) Reuse `cps_prompt.c`'s DK runtime directly.** Its `run`/`copy`/`append`/
  marshal are generic, but its frames are C function pointers
  `DKFrame(intptr_t env, intptr_t value)`. The interpreter's frames are turi
  expression fragments / closures, so this hits the **same C-callback-vs-turi-
  closure mismatch** as the map/set HAMT Gap 2
  ([turi-map-set-hamt-interpreter-gap.md](turi-map-set-hamt-interpreter-gap.md)):
  a turi-closure-aware DK frame variant is required, which collapses (B) back
  into the bespoke work of (A).
- **Decouple the `TUR-E0706` negative path.** Independently of the positive
  capture work, the two not-capturable error fixtures can be recovered by
  running the capturability check (a `collect_ctx` pass) during a phase the
  interpreter executes (elaboration), rather than only in codegen. That is a
  smaller, separable slice that would clear the 2 TI3.2 denylist entries even
  before serial/cloneable resume is implemented.

## How to validate a fix

Run the context-capturing fixtures under the interpreter and compare to their
`expected.stdout`:

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/serial-context-marshal/input.tur
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/cloneable-context-multishot/input.tur
```

The gating positive fixtures and what each one demands (note the multi-shot and
marshaling requirements that rule out a fiber substrate):

| Fixture | Exercises |
| --- | --- |
| `serial-context-marshal` / `-let` / `-if` | `tur_serial_cont_serialize`/`_deserialize`/`_resume`; arithmetic + let + if context frames |
| `serial-context-call1` | 1-arg call frames; `save-cont!`/`resume-cont!` round-trip |
| `serial-context-do` / `-do-cfg` / `-do-struct` | `do`-sequence prelude (run once) + tail continuation; **multi-shot** (`twice`) |
| `cloneable-context-multishot` | in-process **multi-shot** resume (no marshaling) |
| `cloneable-context-let` / `-if` / `-if-outer-frames` | cloneable context-frame grammar |
| `context-call-frame` / `context-division` | call/arithmetic frame capture |

Add the now-passing fixtures to the TI3 block of `tests/run-turi.sh`. Note that
`cont-flavors`, `callcc-*`, and `escape-*` additionally need `call/cc` /
`escape` (`EX_CALLCC`), which the plan tracks separately under the CPS-transform
category and is out of TI3 scope.

---

## Appendix: original (superseded) analysis

The framing below predates the 2026-06-12 implementation read and is **kept for
history only** -- it incorrectly proposed a fiber-backed one-shot `serial-shift`.
See the corrected Root cause and Proposed fix directions above.

> **Two relevant complications (original):**
>
> 1. *The `(k w)` application lowering.* Applying a continuation value of type
>    `serial-cont` / `cloneable-cont` is sugar (cps-transform-plan CC4) that the
>    compiler lowers to the appropriate `*_resume` runtime call. The interpreter
>    needs a matching resume path. The existing effect machinery already exposes
>    `TURI_EFFECT_CONT` + `eval_resume_cont` (used by algebraic-effect handlers),
>    which is the natural substrate to reuse for one-shot resume.
> 2. *Multi-shot capture (`cloneable-shift`).* Resuming the same continuation
>    more than once requires either (a) cloning the suspended fiber stack
>    (`ucontext` copy + register/stack-pointer fix-up), or (b) a replay strategy
>    that re-evaluates the reset body with the shift site injecting each
>    successive resume value. The compiled path sidesteps both by reifying the
>    continuation as a heap `DK` chain (`dk_copy_range`).
>
> **Proposed fix directions (original):**
>
> - *One-shot (`serial-shift`):* reuse the fiber-backed `TuriEffectCont`
>   substrate; model a `serial-reset` as a prompt that runs its body in a fiber;
>   on `serial-shift`, suspend the fiber and call `f` with a `TURI_EFFECT_CONT`
>   wrapping it; `(k w)` resumes the fiber once. *(Wrong: serial continuations
>   are marshalable DK chains, and `serial-context-do` is multi-shot; a fiber
>   passes none of the fixtures.)*
> - *Multi-shot (`cloneable-shift`):* port a heap-reified continuation chain or
>   restrict to pure-context replay.
