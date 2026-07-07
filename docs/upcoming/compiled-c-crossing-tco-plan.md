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

**Status: handler-chain step landed; D1a transport wired behind an experiment.**
The heap handler chain is always-on (see "D1 -- what landed" below). The
return-path-signal transport (D1a) is now implemented behind
`--enable=panic-return-signal` (see
[panic-return-signal-plan.md](./panic-return-signal-plan.md)); it is a prototype
because it does not yet cover the fiber/effect/cancel unwinds and, as the
measurements below establish, does not on its own reach the *nested* shape's
target -- that is bounded by the frame-count wall the D3 stackless lowering
removes (scoped in
[compiled-catch-unwind-stackless-plan.md](./compiled-catch-unwind-stackless-plan.md)).

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

#### D1 -- what landed (handler chain), with measurements

The **handler-chain** half of D1 shipped. Handler discovery now runs off a
thread-local `tur_handler_chain` of heap-allocated `tur_handler_node`s, each
**owning its own `jmp_buf`**, replacing the single global `global_panic_jmpbuf`
that every `catch-unwind` / `catch-panic-of` boundary previously save/restored
onto its own C-stack frame (a `jmp_buf` plus a `memcpy` copy per level). The
box helpers heap-allocate their node; the thunk-fn helpers keep a stack node;
`tur_panic` / `tur_panic_with` consult the chain (innermost node) instead of the
global; the fiber shim saves/clears/restores the chain pointer instead of the
global buffer. Transport stays `setjmp` / `longjmp`. All ~1440 fixtures stay
green (105 preamble snapshots regenerated); a new
`panic-catch-unwind-nested-deep` fixture (`cu-rec` at 80000) guards the win.

Measured effect on the D1 target shapes (compiled backend, `-O2`, 8 MiB stack):

| Shape | Before D1 | After D1 (handler chain) | Notes |
| --- | --- | --- | --- |
| plain deep non-tail recursion | 200000 OK | 200000 OK | baseline; ~1 small C frame/level |
| `cu-catch-deep` (single catch, deep panic) | 200000 OK | 200000 OK | **already passed** -- `longjmp` panic is O(1) stack; the plan listed it as a D1 target but it needed no work |
| `cu-rec` (deeply NESTED catch-unwind) | SIGSEGV below 50000 | SIGSEGV ~150000 | **~3x deeper**; the win D1 delivers |

**D1a transport decision (prototyped, not yet wired):** hand-lowered C
prototypes of both transports on `fib` (opaque input, reachable panic writer so
the flag branch cannot be optimized away):

- **Status-word fat return** is consistently **~3.5x slower at `-O2`** (0.71s
  vs 0.20s baseline) -- exactly the register-pressure hit the plan predicted.
- **Thread-local flag** is at worst neutral and lets the optimizer schedule the
  split-temporary call form better; micro-timing at `-O2` is too heuristic-
  sensitive to quote a single overhead number, but it never regressed baseline.

=> **If/when the return-path signal is wired, use the thread-local flag, not the
status word.**

**Key correction to the D1 premise (important for D3 scoping):** the
return-path signal alone does **not** make deeply *nested* `catch-unwind`
(`cu-rec`) unbounded, and does not even reach 200000. A flag-transport prototype
(no `jmp_buf`, no `longjmp` at all) still SIGSEGV'd at ~120000-150000 -- the same
order of magnitude as the landed handler chain. The wall is **frame *count*,
not frame *size***: `(do (catch-unwind (fn [] (cu-rec (- n 1)))) n)` keeps **two
live C frames per nesting level** -- the boundary's own frame and the "return
`n` after the catch" continuation -- neither of which a return-path signal
removes. Only heap-allocating those continuations (a stackless / partial-CPS
lowering of the boundary, i.e. **D3-class work**) makes the nested shape
unbounded. The signal transport is still worth doing (it removes the `jmp_buf`
frame *size*, roughly doubling depth on its own, and is a prerequisite for
tail-calling across the boundary), but it should be scheduled together with the
stackless-continuation work rather than sold as the thing that hits D4's
1,000,000-deep target for `cu-rec`. The wiring itself is also large: codegen
does **not** A-normalize calls (panic-capable calls are emitted mid-C-expression,
e.g. `1 + deep(n-1)`), so a per-call-site flag check first requires hoisting
every panic-capable call to its own statement -- an ABI-wide transform with
heavy fixture churn.

Net: D1 lands the heap handler chain (a real, measured ~3x nesting-depth win and
the payload-on-node / interleaving-correctness the plan called for). D1a's
transport is decided (thread-local flag) and now **wired behind
`--enable=panic-return-signal`**: `panic` sets `tur_panicking` and returns, every
panic-capable call site is A-normalized to a temp with an `if (tur_panicking)
return <zero>` propagation check, and `catch-unwind` consumes the flag after the
thunk (no `setjmp`). Validated: the default (flag-off) codegen is byte-identical
(1953 fixtures green); with the flag on the `panic-*` catch fixtures keep their
ok/err semantics and `cu-catch-deep` propagates a panic through 200000 non-tail
frames. Measured caveat that confirms the analysis: signal-mode `cu-rec` reaches
the *same* ~150000 as the D1 heap chain, **not deeper** -- because D1 already
moved the `jmp_buf` off the frame, so removing `longjmp` adds no nesting depth;
its value is eliminating `longjmp` and enabling tail-calls across the boundary
(the D3 prerequisite), not depth. The nested-shape 200000/1,000,000 target
requires the stackless-continuation work in
[compiled-catch-unwind-stackless-plan.md](./compiled-catch-unwind-stackless-plan.md).

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
