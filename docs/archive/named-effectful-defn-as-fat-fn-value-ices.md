# A named effectful `defn` used as a `^fat` fn-value ICEs the emitter

> **RESOLVED 2026-07-30.** Boxing a function into a fat closure was counted as
> *performing* that function's effect. Taking an address runs nothing. One-line
> fix plus a plumbing field; details in *Resolution* at the end.

**Severity:** medium (compiler ICE, loud). It aborted at `emit-c` time with an
internal-error message rather than miscompiling, so nothing shipped wrong -- but
a perfectly reasonable program could not be built, and the message named an
internal invariant rather than anything the user wrote.

Verified against `./build/tur` at **v0.32.2** (Debug), on branch
`claude/composite-type-alias-gap-yd70bn`.

## Summary

Passing a **named** effectful `defn` as the value for a **`^fat`** fn-typed
parameter aborted the compiler, unless the call happened to be the *sole* body
of its enclosing `handle`:

```
tur: internal error: effect form (EX kind 57) reached the direct/fiber emitter
  (fiber effect runtime deleted)
```

All four conditions were required. Changing any one made it compile:

| Variant | Result |
|---|---|
| named effectful defn, `^fat` param, call inside a `do` | **ICE** |
| named effectful defn, `^fat` param, call is the sole `handle` body | compiles |
| **inline lambda** instead of the named defn, otherwise identical | compiles |
| named **non-effectful** defn, otherwise identical | compiles |
| named effectful defn, **no `^fat`** on the param | compiles |

Wrapping the call's result in a builtin -- `(println (use-fn logit 4))` --
triggered it the same way `do` did, so in practice any real use site hit it.

## Repro

```turmeric
(defeffect Log [n : int] : nil)

(defn logit [x : int] #fx{Log} : nil
  (perform (Log x)))

(defn use-fn [^fat lg : (fn [int] #fx{Log} nil) n : int] #fx{Log} : int
  (do (lg n) 0))

(defn main [] : int
  (handle
    (do (use-fn logit 4) 0)          ; <-- the `do` is the trigger
    (Log [n] k) (do (println n) (resume k nil))))
```

Remove the `do` and make `(use-fn logit 4)` the whole handle body, and the same
program compiled and ran.

## Root cause

The abort itself (`src/compiler/emit_expr.c:7937`) is a correct refusal: the v2
invariant says a `perform` lowers only on the DK backend, so an `EX_PERFORM`
reaching the direct emitter is a bug. The real question was why `logit`'s body
got routed there at all. `TUR_TRACE_EVICT=1` gave the chain:

```
[EVICT] BODY-STRUCT-OR-TAINT   eff=1 logit
```

`BODY-STRUCT-OR-TAINT` means no un-lowerable form in `logit` itself -- it was
co-evicted. Tracing the classification fixpoint:

1. `main` failed `term_core_ok` **structurally** (no `CT_UNSUPPORTED` node).
2. So `main` became a fiber function, which **tainted** the `Log` effect.
3. The taint co-evicted every `Log` peer -- including `logit`, the performer.
4. `logit`'s `perform` then reached the direct emitter. ICE.

The structural rejection was a `CT_APPCONT` with `kont.kind == KK_PROMPT` --
delivering a plain value to the enclosing prompt, which is exactly how
`(handle (do ... 0) ...)` ends. `term_core_ok` refuses that by design; the
relaxed `handle_delim_ok` exists to permit it inside a handled body. So the
question narrowed again: why did the handled body fall back to `term_core_ok`?

Because of its `CT_LETRAW` case:

```c
case CT_LETRAW:
    if (letraw_ok(t) && letraw_effect_free(t))
        return handle_delim_ok(t->as.letraw.body);
    return term_core_ok(t);          /* <-- taken */
```

The handled body opens with a `CT_LETRAW` that builds the **fat-closure box for
`logit`** -- a `malloc`, the shim in slot 0, `logit`'s address in slot 1. That
runs no effect. But `letraw_effect_free` asked the question via the shared
`EffAcc` callee list, and that list deliberately records a bare `EX_VAR`
reference to a fn-typed binding as a "callee" -- correctly, for its *other*
consumer, the call-path taint, which needs to know a fn "may be CALLED
downstream through the value" (`emit_cps_ir.c:3443-3458`).

Two different questions, one list. For a `CT_LETRAW` admission the question is
"does this raw op run an effect *right here*, on the fiber" -- and taking a
function's address never does. So the box construction was rejected as
effectful, and everything above followed.

`^fat` mattering is what made the trigger look so arbitrary: only a `^fat`
parameter forces the caller to box the fn-value, which is what creates the
`CT_LETRAW` in the first place. An inline lambda is lifted and boxed elsewhere,
so it never produced this node.

## Resolution -- 2026-07-30

`EffAcc` gains a `calls_only` flag that suppresses the fn-VALUE reference
channel (the `EX_VAR`-on-`TY_FN` callee record). `letraw_effect_free` is the
only caller that sets it, so it now counts **call-position callees only**.

A genuine delegated *call* to an effectful callee inside a handled body is still
rejected -- that is the case the gate exists for, and the reason its comment
cites `handle-effectful-fn-param-same-fn`: such a call would run its effect on
the fiber runtime, escaping the handle's DK prompt. Only the address-reference
false positive is removed.

### Verification

Every previously-ICEing shape now compiles **and runs with the effect correctly
handled** -- the concern with admitting `main` was that the effect might instead
escape the prompt at runtime, so this was checked end to end, not just to
`emit-c`:

| Shape | Before | After |
|---|---|---|
| call inside a `do` | ICE | handler receives the perform, resumes |
| result nested in `println` | ICE | same |
| tail-position call inside a `do` | ICE | same |
| sole `handle` body | compiled | unchanged |

Regenerating the codegen snapshots produced **zero** changes: the relaxation
only admits shapes that previously evicted, so no existing program's lowering
moved. `bash tests/run.sh`: **2442 passed, 0 failed**.

`tests/fixtures/effectful-fat-fn-param-named/` was written around this
limitation while fixing the sibling SIGSEGV; it now exercises all three
previously-ICEing positions instead of avoiding them.

### Note for whoever revisits the v2 invariant

The comment at `emit_expr.c:7930` calls the direct-emitter `perform` case
"corpus-verified unreachable". It was reachable, via co-eviction rather than
via any single un-lowerable form -- so the ICE is doing its job and should stay,
but "unreachable" is too strong. Any future taint rule that can evict a
performer can resurrect it.

## Found while

Adding fixture coverage for
[effectful-fn-typed-param-call-segfaults.md](effectful-fn-typed-param-call-segfaults.md)
(the `^fat` + effect-row SIGSEGV, fixed on the same branch). Confirmed
orthogonal to that fix before diagnosing: it aborted identically on a compiler
rebuilt with the fix stashed.
