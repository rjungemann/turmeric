# Type ascription on a closure-captured variable emits the raw name, not the env access

**Summary:** `(:: v :T)` where `v` is a variable captured by an enclosing
closure miscompiles -- the codegen emits the bare local name (e.g. `ch_1024`)
instead of the captured-environment access (e.g. `__env___env_1028->ch`),
producing a C `'<name>' undeclared` error inside the lifted closure function.

**Severity:** Hard compile error (cc fails), but the more worrying shape is that
the ascription wrapper silently defeats the closure-capture rewrite. Any value
that reaches a closure body only through a `(:: ...)` ascription is at risk.
Severity: medium-high (hard error here; latent miscompile risk for other
capture-rewrite passes that key off the bare variable node).

## Minimal repro

```turmeric
(defopaque Chan :ptr<void>)

(defn chan-new [cap : int] : Chan
  ```c return (int64_t)(intptr_t)malloc(8); ```)

(defn use-raw [p : ptr<void>] : int
  ```c return (int64_t)(intptr_t)p; ```)

(defn spawn [f : ptr<void>] : nil
  ```c ((void(*)(void*,int64_t))((int64_t*)f)[0])((void*)f, 0); ```)

(defn main [] : int
  (let [ch (chan-new 4)]
    (spawn
      (fn [user : int] : nil
        ;; ch is captured here; the (:: ch :ptr<void>) ascription breaks the
        ;; capture rewrite and emits the bare `ch_NNNN` local instead of the
        ;; `__env->ch` field.
        (use-raw (:: ch :ptr<void>))
        0))
    0))
```

Observed (from the real case in
`tests/fixtures/reactor-fibers-park-chan` after the Tier-1 chan handle
newtypes landed):

```
.../input_tur.c: In function '__fn_1026':
.../input_tur.c:3240:95: error: 'ch_1024' undeclared (first use in this function)
   print_got(local_park_chan((void *)(intptr_t)(__env___env_1028->g),
                             (void *)(intptr_t)(ch_1024)));
```

Note the sibling argument `g` -- captured the same way but used *without* an
ascription -- is correctly rewritten to `__env___env_1028->g`. Only the
ascribed `ch` regresses to the bare name.

**Expected:** the ascribed variable should resolve to the same captured-env
access as any other use of that variable inside the closure, i.e.
`(void *)(intptr_t)(__env___env_1028->ch)`.

## Root cause (hypothesis)

The closure free-variable / capture-rewrite pass appears to walk for bare
variable-reference nodes and rewrite them to env-field accesses. The `(:: expr
:T)` ascription node is a thin relabel (it lowers to a no-op cast for defopaque
/ same-kind types, see `tests/fixtures/opaque-ascribe-int`), but the rewrite
pass does not descend into the ascription's inner expression -- so a variable
that only appears under `(::)` is missed and survives to codegen as a bare
local. Likely fix locations: wherever closure capture collection /
substitution is performed (free-variable scan + the rewrite that swaps
`Var -> EnvAccess`); the ascription/`::` node needs to be transparent to both
the free-variable scan and the substitution.

## Workaround

Hoist the ascription out of the closure so the cast happens in the enclosing
scope and the closure captures the already-converted value directly:

```turmeric
(let [ch  (chan-new 4)
      chp (:: ch :ptr<void>)]   ;; cast outside the closure
  (spawn (fn [user : int] : nil (use-raw chp) 0)))
```

This is what `tests/fixtures/reactor-fibers-park-chan/input.tur` now does.

## How to validate a fix

1. Add the minimal repro above as a happy fixture (it should build, run, and
   exit 0) and confirm the generated C references `__env...->ch` rather than a
   bare `ch_NNNN` inside the lifted closure.
2. Revert the hoist workaround in
   `tests/fixtures/reactor-fibers-park-chan/input.tur` (inline the
   `(:: ch :ptr<void>)` back into the receiver closure) and confirm the suite
   still passes.

## Discovered while

Implementing Phase 1 (chan only) of
`docs/upcoming/stdlib-opaque-handle-types-plan.md` -- introducing the
`Chan`/`AsyncChan` `defopaque` handle newtypes, which forced an explicit
unwrap cast at the still-untyped `tur/reactor` boundary and surfaced the bug.
