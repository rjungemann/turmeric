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

## The blocker -- continuation representation mismatch

The obstacle is that the two surfaces spell "resume" differently:

- A shift receiver is `(fn [k : cont] (k v))` -- `k` is `cont`-typed and resumed
  with the `(k v)` application sugar (`tur_cloneable_cont_resume` /
  `ContFlavor`).
- An effect handler continuation is resumed with `(resume k v)`; it is NOT
  `cont`-typed -- `(k v)` on it errors ("not a function or continuation").

So the desugar must align them, via either:

1. **A `cont` flavor for effect continuations** (`CONT_EFFECT`): make the
   handler `k` a `cont` whose `(k v)` sugar lowers to `resume`. Then the shift
   receiver `(fn [k : cont] (k v))` works unchanged as the handler body's
   applied receiver. Smallest surface change, but touches the type of handler
   continuations and the `(k v)` dispatch (`elab_call.c` CC4).
2. **Rewrite the receiver's `(k v)` to `(resume k v)`** in the desugar -- more
   local, but requires transforming the receiver body.

Either is a real, multi-part change (synthetic-effect synthesis + handler-body
construction + a continuation-flavor/rewrite + coexistence with the reified path
so only the cross-function case desugars). It spans elaboration and the type
system and must stay `direct == cps == turi` and suite-green.

## Recommendation

Cross-function resume is the deep first-class-continuations work the parent plans
flagged as "a plan of its own." It should be scoped as its own change along the
`__Shift`-effect-desugar design above, not folded into the incremental
unification slices. Until then:

- The tailored `TUR-E0016` message steers users to a lexical reset or to
  effects (which already give cross-function resumable continuations).
- Fixture `errors/shift-crossfn-resume` pins the diagnostic.
