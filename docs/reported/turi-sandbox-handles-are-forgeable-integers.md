# Sandboxed interpreter: handles are forgeable integers, and some paths end the host

**Severity:** high under T3 (the sandboxed interpreter) and under T1 for
`tur check` (the macro environment). Tracked as **S-5** in
[security-audit-plan](../upcoming/security-audit-plan.md). Filed 2026-09-30 by
WP3, which closed the capability half of the sandbox (S-1) and found this
underneath it.

A capability-denied environment (`turi_env_new_sandboxed()`, the macro
environment) can no longer reach the OS through a native: every native carries
a required capability and the native dispatch checks it. What it can still do
is corrupt the host's memory, or end the host process, with no capability at
all.

## Repro

Against a Debug build, from an embedder:

```c
TuriEnv *env = turi_env_new_sandboxed();
turi_eval(env, "(vec-get 4096 0)");
```

```
AddressSanitizer: SEGV on unknown address 0x000000001008 ... READ memory access
```

The same shape crashes through `(tur_hamt_count 4096)` and `(vec-len 4096)`.
`vec-set!`, `mutmap-set!` and the HAMT setters make it a write. From
`tur check`, the same call inside a `defmacro*` body runs in the compiler's own
process.

End the host process:

```c
turi_eval(env, "(panic \"x\")");                      /* exit status 1 */
turi_eval(env, "(let [v (vec-new)] (vec-get v 5))");  /* "vec index out of bounds", _exit(1) */
```

## Root cause

The interpreter carries every collection, string-builder, continuation and
cons handle as a `TURI_INT` holding a pointer, and the natives cast it back
with no check that it came from the matching constructor, e.g.
`native_vec_get` (`src/turi/collections_native.c`, the `(int64_t *)(intptr_t)
a[0].as_int` at the top of the function). `head`, `int-val`, `unbox`,
`cstr-free`, the `tur_*_cont_*` builtins (`ts_try_cont_builtin` in
`src/turi/eval.c`) and several hundred more share the shape.

The natives resolve at runtime by name (TUR-W0040, "will runtime-dispatch"),
so the elaborator's types never stand between the text and the cast.

The host-exit half is the native error paths that call `_exit` (the
bounds-checked `vec-get` in `collections_native.c`) and the `panic` builtin,
which is the compiled program's semantics carried over unchanged.

## Why WP3 did not fix it

It is not a capability. Gating every handle-taking native behind
`TURI_CAP_UNSAFE` would deny vectors, maps and strings to a sandbox, which is
the sandbox being useless rather than safe. WP3 gated only the natives whose
sole purpose is raw memory (`box`, `unbox`, `io-alloc`, `int-val`, `flat-*`,
`array-get`/`array-set`, ...; see the classification table in
`src/turi/native_caps.c`).

## Fix directions

1. **Tag handles.** Give interpreter handles their own `TuriTag` (or a boxed
   `TURI_HANDLE` carrying a kind and a generation), have each constructor
   return one, and have each native check the kind before the cast. Largest
   change, and the only one that closes the class.
2. **A handle registry per env.** Constructors record every pointer they hand
   out in a per-env set; natives look the integer up before casting. Smaller
   change, costs a hash lookup per native call, and only needs doing when
   `env->caps != TURI_CAP_ALL`.
3. **Host exit.** Under a capability-restricted env, turn `panic` and the
   `_exit` error paths into `TURI_ERROR` returns (a `longjmp` back to
   `turi_eval`, which the r7rs `call/cc` path already has machinery for),
   so the embedder decides.

Direction 2 plus 3 is the cheapest route to making the T3 promise and the
deferred T1 macro promise in `docs/guides/security-guide.md`.
