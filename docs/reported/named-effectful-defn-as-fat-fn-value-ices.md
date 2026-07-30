# A named effectful `defn` used as a `^fat` fn-value ICEs the emitter

**Severity:** medium (compiler ICE, loud). It aborts at `emit-c` time with an
internal-error message rather than miscompiling, so nothing ships wrong -- but a
perfectly reasonable program cannot be built, and the message names an internal
invariant rather than anything the user wrote.

Verified against `./build/tur` at **v0.32.2** (Debug), on branch
`claude/composite-type-alias-gap-yd70bn`.

## Summary

Passing a **named** effectful `defn` as the value for a **`^fat`** fn-typed
parameter aborts the compiler, unless the call happens to be the *sole* body of
its enclosing `handle`:

```
tur: internal error: effect form (EX kind 57) reached the direct/fiber emitter
  (fiber effect runtime deleted)
```

All four conditions are required. Change any one and it compiles:

| Variant | Result |
|---|---|
| named effectful defn, `^fat` param, call inside a `do` | **ICE** |
| named effectful defn, `^fat` param, call is the sole `handle` body | compiles |
| **inline lambda** instead of the named defn, otherwise identical | compiles |
| named **non-effectful** defn, otherwise identical | compiles |
| named effectful defn, **no `^fat`** on the param | compiles |

Wrapping the call's result in a builtin -- `(println (use-fn logit 4))` --
triggers it the same way `do` does, so in practice any real use site hits it.

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
program compiles and runs.

## Root cause direction

The abort is at `src/compiler/emit_expr.c:7937`. It fires when an `EX_PERFORM`
(or `EX_WITH_HANDLER`) reaches the direct/fiber emitter, which the v2 invariant
says is unreachable -- a `perform` is supposed to lower only on the DK backend
(`emit_cps_ir.c`). The comment there calls the case "corpus-verified
unreachable"; this repro is a counterexample.

So the question is not why the direct emitter mishandles `perform` (it
correctly refuses to), but **why `logit`'s body got routed to the direct emitter
at all**. Something about taking a named effectful defn's address as a `^fat`
value evicts it from the CPS backend, and the eviction does not account for the
body still containing a `perform`. `src/compiler/emit_stmt.c:447` discusses
exactly this shape -- "a colored function evicted from the CPS ... direct/fiber
emitter" -- and is the place to start.

The `do`/no-`do` split is a strong hint that the classification depends on the
call's syntactic position rather than on the callee's effect row: the sole-body
position presumably keeps the whole chain colored, while sequencing it lets
`logit` fall out.

## Fix directions

1. **Find the eviction and exclude effectful callees from it.** A function whose
   body performs must stay colored regardless of how its value is taken. This is
   the real fix.
2. **Failing that, diagnose instead of ICE.** If some shapes genuinely cannot be
   colored, a `TUR-E` at check time naming the restriction ("a named effectful
   defn cannot be passed as a `^fat` fn-value here") is far better than an
   internal-error abort in the emitter -- the user has no way to connect
   "EX kind 57" to the line they wrote.

## Found while

Adding fixture coverage for
[effectful-fn-typed-param-call-segfaults.md](../archive/effectful-fn-typed-param-call-segfaults.md)
(the `^fat` + effect-row SIGSEGV, fixed on this branch). Confirmed pre-existing
and orthogonal: it aborts identically on a compiler rebuilt with that fix
stashed. `tests/fixtures/effectful-fat-fn-param-named/` is shaped around this
limitation -- its call is the sole `handle` body deliberately.
