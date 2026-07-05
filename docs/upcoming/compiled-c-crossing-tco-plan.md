---
title: C-crossing TCO in the compiled backend (heap-bounded catch-unwind / atomically / async) -- Plan
category: Planning
description: The compiled backend currently C-recurses through the same three boundary forms the interpreter's turi-c-scoped-forms-heap-bounding-plan.md targets -- `catch-unwind` (setjmp landing pad), `atomically` (STM tx C frame), and `async`/`await`/`handle` (ucontext fibers). A program that nests any of them ~200000 deep SIGSEGVs the compiled binary. This plan scopes the ABI-level work that would let the compiled path heap-bound them too, restoring semantic parity with the interpreter. The work is genuinely deep (touches the calling convention and unwinding path) and is deliberately not on the near-term roadmap; it exists so the interpreter's superset stance is documented as temporary, not permanent.
---

# C-crossing TCO in the compiled backend -- Plan

## Why this exists

The sibling plan [turi-c-scoped-forms-heap-bounding-plan.md](./turi-c-scoped-forms-heap-bounding-plan.md)
heap-bounds `catch-unwind`, `atomically`, and `async`/`await`/`handle` in the
**tree-walking interpreter** by modeling each on the work-stack driver. When it
lands, the interpreter will run those forms 1,000,000 deep; the compiled backend
will still SIGSEGV the same input at ~200000. That divergence is real and
worth closing on its own terms -- but the mechanism is ABI-level in the
compiled path (calling convention + unwinding), not a peephole. This plan
scopes it end-to-end so the "interpreter is a superset" stance is understood
as temporary.

## Why the compiled path C-recurses today

Each of the three forms lowers to a C construct that is intrinsically C-stack-scoped:

- **`catch-unwind`** compiles to `setjmp` + a C frame that owns the `jmp_buf`,
  and `panic` compiles to `longjmp`. The setjmp frame *is* the boundary; a tail
  call across it either grows a new C frame (defeating TCO) or deletes the
  setjmp frame (making a subsequent `longjmp` undefined).
- **`atomically`** compiles to a C wrapper that owns the STM transaction log
  (`g_stm_tx` and friends) on its own C frame; the body runs beneath it and a
  `retry` re-enters the wrapper.
- **`async`/`await`/`handle`** run on `ucontext` fibers -- each fiber has its
  own C stack. Intra-fiber TCO already works; nested suspensions each cost a
  fresh ucontext.

LLVM's `invoke`/`landingpad` and Itanium-ABI unwinding do not fix `catch-unwind`
on their own: a tail call across a `try` still deletes the frame the landingpad
is attached to, so a subsequent panic would land in the *caller's* catch, not
the one the source-level `catch-unwind` named. Handler discovery has to move
off the C stack regardless.

## The shared mechanism (mirror of the interpreter's driver-signal move)

The interpreter plan's shape -- replace a C-stack boundary with a heap-anchored
control structure plus a return-path signal check -- is the same shape the
compiled backend needs, just realized in codegen and calling-convention terms
instead of driver-frame terms. The three phases map one-to-one.

### Phase D1 -- `catch-unwind` on a heap handler chain

- Add a thread-local `tur_handler_chain` -- a linked list of heap-allocated
  handler nodes, each carrying the saved defer mark, module state,
  no-unwind flag, and a pointer to the target continuation (the compiled
  address to resume at when the panic is caught).
- Lower `catch-unwind` to: push a handler node, call the thunk (a plain tail-
  callable call -- no setjmp frame pinning), pop the node on normal return
  and wrap the result; on caught panic, jump to the stored continuation with
  the payload.
- Lower `panic` to: fill the payload on the top handler node, set a
  thread-local `tur_panicking` flag (or return a status word -- see D1a),
  and return to the caller. Every compiled call site checks the flag on
  return; if set, it propagates (returns immediately) until a frame
  registered as a handler consumes it.
- **D1a -- signal transport.** Two options, decided empirically:
  - **Thread-local flag** (`tur_panicking`): simple, one check per return
    site, but every function pays the check even in the non-panic path.
  - **Status-word return** (`{value, status}` fat return): checked in the
    ABI without a global load, but changes every function signature and
    hurts LLVM's ability to keep small returns in registers.
  - Prototype both on a small benchmark (fib + panic-free hot loop) before
    committing. Rust-style zero-cost DWARF unwinding is a third option but
    incompatible with tail calls across the try (see "Why the compiled
    path C-recurses today").
- Payload layout matches the interpreter's existing `catch_panic_*` fields:
  msg / type / value / file / line. The compiled runtime already has these
  as globals in the panic path; move them onto the handler node.
- Defers: the handler node captures the defer-stack mark at push time; a
  caught panic fires unwound defers (LIFO) *before* jumping to the
  continuation, exactly as the interpreter's `catch-unwind` W4 fix does.

### Phase D2 -- `atomically` on a heap-anchored tx-log

- Move `g_stm_tx` and the transaction log off the C stack onto a
  heap-allocated transaction node, hung off the thread. The compiled
  wrapper becomes a thin stub that pushes the node, tail-calls the body,
  and handles retry by re-entering the body without a new C frame.
- `retry` is a status like `tur_panicking`: raised on the return path,
  consumed by the transaction node's owning stub, which re-drives the body.
- Reconcile lifetime: the node lives as long as the transaction; nested
  transactions push/pop the node chain.
- Depends on D1's signal-transport choice (D1a) -- retry rides the same
  mechanism as panic.

### Phase D3 -- `async` / `await` / `handle` continuations

- Largest piece and least concrete. Two directions, pick after D1/D2 land
  and measure:
  - **(a) Bound the per-suspension cost.** Keep fibers, but ensure each
    fiber's own body is TCO'd (D1/D2 already deliver this) and shrink the
    per-suspension ucontext cost. Bounds the growth per suspension but
    does not eliminate C-stack use per active fiber.
  - **(b) First-class continuations / CPS transform.** Replace ucontext
    with heap-allocated continuations built by the codegen (or by a
    partial CPS transform of the effect/async surface). This is a real
    backend rewrite, comparable in scope to D1 and D2 combined.
- Not scoped further here -- D3 gets its own plan once D1/D2 land and the
  measured growth pattern is clear.

## Phase D4 -- match the interpreter's audit at 1,000,000 deep

Once D1-D3 land, run the same probes the interpreter plan's C4 uses
(`cu-rec`, `atom-rec`, `fiber-rec`) against the compiled binary; success
means both backends run them 1,000,000 deep with no SIGSEGV. At that point
the interpreter's superset stance retires and the two backends are once
again observationally equivalent for these forms.

## Cost estimate (signals, not measurements)

- **Return-path signal check** (D1a): low single-digit % overhead on the
  non-panicking hot path if implemented as a thread-local flag load + branch
  per call site. Status-word return may be cheaper or costlier depending on
  how LLVM handles the fat return; needs a real measurement.
- **Heap handler node allocation**: one small alloc per `catch-unwind` push.
  Pool-allocatable per thread; not on the hot path unless catch-unwind is
  hot.
- **D3**: unknown; depends on direction chosen.

## Validation

`bash tests/run.sh` (regenerate the baseline on the day the phase lands) with
the 10-minute timeout, plus a compiled-side audit matching `tests/turi/eval-tco`.
The regressions per phase mirror the interpreter's SR-style slices: same
programs (`cu-rec`, `atom-rec`, `fiber-rec` at 200000 during development,
1,000,000 for the D4 sign-off).

## Out of scope

- Changing the source-level semantics of any of the three forms. This plan is
  strictly about *how deep they can nest* under the compiled backend, not
  what they mean.
- Rewriting the effect system. D3 is a *codegen* direction for the existing
  effect/async surface, not a redesign of it.
- Retrofitting the change onto older releases. This is a v-boundary ABI
  change; land it once and move forward.
