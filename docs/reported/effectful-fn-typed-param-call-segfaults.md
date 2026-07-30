# Calling a fn-typed parameter whose type carries a non-empty effect row segfaults

**Severity:** high (silent miscompile -> crash). It type-checks clean, emits C
that compiles clean, and dies at runtime with no diagnostic. Any API that
declares a callback as effectful -- middleware, a logging hook, a visitor that
may `perform` -- is unusable.

Verified against `./build/tur` at **v0.32.2** (Debug), on branch
`claude/composite-type-alias-gap-yd70bn`.

## Summary

A parameter typed `(fn [T] #fx{E} R)` with a **non-empty** effect row crashes
the moment it is called. Neither a `handle` nor an actual `perform` is
involved -- the callee never performs anything and there is no handler in the
program. An **empty** row `#fx{}` is fine, and so is no row at all.

| Parameter type | Result |
|---|---|
| `(fn [int] nil)` | works |
| `(fn [int] #fx{} nil)` | works |
| `(fn [int] #fx{Log} nil)` | **SIGSEGV** |
| `(fn [int] #fx{Log} int)` | **SIGSEGV** |

The passed value's shape does not matter: an inline lambda and a named top-level
`defn` both crash.

## Repro

```turmeric
(defeffect Log [n :int] :nil)

(defn use-fn [^fat lg : (fn [int] #fx{Log} nil) n : int] #fx{Log} : int
  (do (lg n) 0))

(defn main [] : int
  (use-fn (fn [x : int] nil) 42))
```
```
$ tur check q1.tur      # clean, exit 0
$ tur run q1.tur
Segmentation fault
```

Control -- identical but with an empty row -- runs fine:

```turmeric
(defn use-fn [^fat lg : (fn [int] #fx{} nil) n : int] : int
  (do (lg n) 0))

(defn main [] : int
  (use-fn (fn [x : int] nil) 42))
```

## Root cause

The non-empty row makes the emitter CPS-convert the call and route it through
the E2a direct-entry -> CPS-entry registry. From `tur emit-c` on the repro:

```c
static int64_t use_hyfn__cps(int64_t lg, int64_t n, DK *__kont) {
    return ((int64_t (*)(int64_t, DK *))__tur_cps_lookup((intptr_t)lg))(
        n, __dk_reap_node(dk_frame(use_hyfn_j0, 0, __kont)));
    /* E2a threaded fn-value heap join */
}
```

versus the empty-row control, which uses the ordinary fat-shim call:

```c
(*(tur_thunk_void_int64_t_t *)((void *)(intptr_t)(lg)))((void *)(intptr_t)(lg), n);
```

Two things go wrong in the first form:

1. **The key is wrong.** `__tur_cps_lookup` is keyed by a *direct top-level
   entry point* address, registered by `__tur_cps_register`. `lg` is a fat
   closure handle -- a `malloc`'d `[shim, fnptr]` record built at the call site
   (visible in the emitted `main`) -- so its address was never registered.
   This is also why passing a named `defn` does not help: it is boxed into the
   same fat handle before the call.

2. **The miss is unguarded.** `__tur_cps_lookup` returns `(__tur_cps_fn)0` when
   nothing matches, and the emitted code calls the result immediately with no
   NULL check. A registry miss is therefore a call through a NULL pointer
   rather than a diagnostic.

The registry and both call shapes are in the codegen preamble; the call-site
selection is in the emitter's fn-value application path (search for the
`E2a threaded fn-value heap join` comment string).

## Fix directions

1. **Make the miss loud, not fatal** (cheapest, do this regardless). Guard the
   `__tur_cps_lookup` result and abort with a named runtime error instead of
   calling NULL. This converts a silent SIGSEGV into something diagnosable and
   would have made this report a five-minute find rather than a bisect.
2. **Key the lookup off the boxed handle.** A fat handle already carries the
   underlying function pointer in slot 1; look *that* up rather than the
   handle address. Fixes the named-`defn` case immediately; an inline lambda
   still needs its CPS entry registered.
3. **Register CPS entries for lambdas that flow into an effectful fn-typed
   parameter.** The emitter knows the parameter's declared type at the call
   site, so the lambda's lifted body can be registered alongside its direct
   entry.
4. **Or: reject the shape at check time** until 2/3 land. An effectful
   fn-typed parameter that cannot be called is worse than one that will not
   compile, and a `TUR-E` naming the restriction is a strictly better outcome
   than the current crash.

Fix 1 is independent of the rest and worth landing on its own.

## Found while

Validating that `defalias` composite targets are transparent for
[composite-type-alias-gap.md](../archive/composite-type-alias-gap.md).
`(defalias Logger (fn [cstr] #fx{Log} nil))` crashed, and the control with the
type spelled inline crashed identically -- so this is orthogonal to aliases.
It is why `tests/fixtures/defalias-composite/` uses a pure `(fn [int] int)`
rather than the effect-annotated shape from
`tests/fixtures/effect-type-alias/`, whose `deftype Logger` is declared but
never used as an annotation.
