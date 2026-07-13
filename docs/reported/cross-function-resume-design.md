# Cross-function resume (item c) -- design + scope

**Severity: low (design note -- item (c) of the resuming-shift plan is the deep
first-class-continuations piece; this records the tractable path and why it is a
substantial effort, not a bounded increment).**

## The gap

After the unification (items A/B), one `shift`/`reset` pair covers abortive,
resumable (lexically scoped), single-shot, and cross-function **abort**. The one
remaining hole is cross-function **resume**: a resuming shift whose receiver
invokes the continuation, delimited by a reset in a *caller*.

```turmeric
(defn ck [k : cont] : int (+ (k 1) (k 2)))
(defn inner [] : int (shift ck 0))          ; resumes k
(defn outer [] : int (reset (+ 10 (inner)))); reset is in the caller
```

This is rejected (`TUR-E0016`, now with a tailored message pointing here). The
reified-context lowering is **lexically scoped** -- the continuation is reified
by walking the reset body syntactically, which cannot cross a function boundary.
Cross-function *abort* works because it never reifies (it aborts dynamically);
cross-function *resume* fundamentally needs a dynamically-captured continuation.

## Key finding -- the machinery already exists (effects)

`perform` / `handle` / `resume` **already do cross-function resumable
continuations**, on both paths (compiled `dk_invoke`; interpreter fibers).
Verified:

```turmeric
(defeffect Yield [] :int)
(defn producer [] : int (+ 100 (perform (Yield))))   ; performs cross-function
(defn run [] : int
  (* 10 (handle (producer) (Yield [] k) (resume k 5))))  ; handler resumes k
;; direct == turi == 1050
```

So cross-function resume is not missing from the runtime -- it is missing only
from the *shift/reset surface*. The tractable design is therefore a
**shift/reset -> effect desugar**, not new DK-subk plumbing across three
backends.

## The design -- shift/reset as a synthetic effect

A resuming shift/reset maps onto one synthetic effect `__Shift` carrying the
receiver:

- `(reset BODY)` (when it binds a cross-function resuming shift) ->
  `(handle BODY (__Shift [recv] k) (recv <k-as-cont>))` -- the handler receives
  the shift's `recv` as the effect payload and the delimited continuation `k`,
  and applies `recv` to it.
- `(shift recv _)` -> `(perform (__Shift recv))`.

This inherits the effect machine's cross-function capture on both paths.

## Blocker 1 -- resume representation mismatch -- RESOLVED

The two surfaces spelled "resume" differently: a shift receiver `(fn [k : cont]
(k v))` uses the `(k v)` sugar (cloneable resume), while an effect handler
continuation used `(resume k v)` and was NOT applicable with `(k v)`.

**Landed:** `(k v)` now works on an effect handler continuation too -- a call
through an `is_continuation` binding produces an `EX_RESUME` (shared
`elab_make_resume`, factored out of `elab_resume`; `elab_call.c` routes the
`(k v)` sugar there). So a receiver `(fn [k] (k v))` resumes a handler
continuation uniformly. Verified cross-function (`perform` in a callee, `handle`
+ `(k 5)` in a caller): `direct == turi == 1050` (fixture
`handler-cont-kv-sugar`). This is the foundation the desugar needs -- the handler
body can now apply the shift's receiver to the handler continuation directly.

Note: this reuses the handler continuation's existing int64 representation (no
`CONT_EFFECT` type flavor needed) -- the `is_continuation` flag is the hook, and
`resume` already accepts an arbitrary continuation *value*.

## Blocker 2 -- reset must catch a cross-function perform -- the remaining work

A `(shift recv _)` in a callee lowers to `(perform (__Shift recv))`; the matching
handler must be at the enclosing `reset`, which is in a *caller* and is
elaborated separately. So **every `reset` must install a `__Shift` handler**
around its body:

```
(reset BODY)  ->  <abortive-or-reified-reset> ( handle BODY
                    (__Shift [recv] k) (recv k) )      ; recv resumes k via (k v)
```

A lexical abortive/reified shift bypasses the handler (it is `EX_SHIFT` /
`EX_CLONEABLE_SHIFT`, not a `perform`) and hits the outer reset unchanged; only a
cross-function resuming shift performs `__Shift` and is caught here. `(recv k)`
applies the receiver to the handler continuation (now possible via blocker 1).

The cost/risk: wrapping every reset in a handler changes reset codegen for the
whole corpus (fixture snapshots, and the shift0 / nested / substrate shapes the
reset-alias experiment showed are fragile). The safe way to introduce it is a
**whole-program pass** that wraps resets in the `__Shift` handler *only when the
program contains a cross-function resuming shift* -- so the common case (no such
shift) is byte-for-byte unchanged. A same-elaboration global flag will not do
(a reset can be elaborated before the callee's shift sets it); it needs a post-
elaboration transform, plus synthesis of the `__Shift` effect (carrying an
fn-value payload -- confirm effects can carry fn payloads) and the perform/handle
nodes.

## Status

- **Foundation landed:** unified resume surface (`(k v)` on handler
  continuations) -- fixture `handler-cont-kv-sugar`.
- **Remaining (its own focused change):** the reset -> `__Shift`-handler
  whole-program transform + `shift` -> `perform` desugar for the cross-function
  resuming case. This is the deep first-class-continuations work; it is gated
  only by the reset-wrapping transform now, not the resume mismatch.
- Until then the tailored `TUR-E0016` message steers users to a lexical reset or
  to effects (fixture `errors/shift-crossfn-resume`), which already give
  cross-function resumable continuations -- now with `(k v)` sugar.
