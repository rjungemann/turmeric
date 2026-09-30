# Sandboxed interpreter: handles are forgeable integers

**Severity:** high under T3 (the sandboxed interpreter) and under T1 for
`tur check` (the macro environment). Tracked as **S-5** in
[security-audit-plan](../upcoming/security-audit-plan.md). Filed 2026-09-30 by
WP3, which closed the capability half of the sandbox (S-1) and found this
underneath it.

**Narrowed 2026-09-30.** The report was filed with two halves. The second, a
restricted env ending the host process, is fixed (see *Resolved: host exit*
below). What is open is the first: forged handles.

A capability-denied environment (`turi_env_new_sandboxed()`, the macro
environment) cannot reach the OS through a native: every native carries a
required capability and the native dispatch checks it. What it can still do is
read or write an arbitrary address in the host's process, with no capability
at all.

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

## Root cause

The interpreter carries every collection, string-builder, continuation and
cons handle as a `TURI_INT` holding a pointer, and the natives cast it back
with no check that it came from the matching constructor, e.g.
`native_vec_get` (`src/turi/collections_native.c`, the `(int64_t *)(intptr_t)
a[0].as_int` at the top of the function). `head`, `int-val`, `unbox`,
`cstr-free` and the `tur_*_cont_*` builtins (`ts_try_cont_builtin` in
`src/turi/eval.c`) share the shape.

**Measured scope (2026-09-30):** of the 656 builtin natives, **204** cast an
integer argument straight to a pointer in their own body. That count is a
floor: it misses natives that hand the integer to a helper (`sbuf_of`,
`json_node_ptr`, the `tur_hamt_*` runtime functions in `src/runtime/`) before
the cast.

The natives resolve at runtime by name (TUR-W0040, "will runtime-dispatch"),
so the elaborator's types never stand between the text and the cast. Types
would not be a sound fix anyway: an erasing ascription or a stdlib `:int`
stand-in launders an integer into a handle type.

## Why it was not fixed with the capability check

It is not a capability. Gating every handle-taking native behind
`TURI_CAP_UNSAFE` would deny vectors, maps and strings to a sandbox, which
makes the sandbox useless rather than safe. WP3 gated only the natives whose
sole purpose is raw memory (`box`, `unbox`, `io-alloc`, `int-val`, `flat-*`,
`array-get`/`array-set`, ...; see `src/turi/native_caps.c`).

## Fix directions

Both directions need a second column in `src/turi/native_caps.c`: for each
native, which argument positions are handles and of what kind, and whether its
result is one. That is the bulk of the work, 200+ rows read by hand, and the
sandbox test can pin it the same way it pins the capability column.

1. **A provenance set per restricted env** (the cheaper route). When
   `env->caps != TURI_CAP_ALL`, the dispatch records each handle a
   constructor-classified native returns, keyed by kind, and refuses a call
   whose handle argument is not in the set for its kind. It costs a hash
   lookup per handle argument, only in restricted envs. Frees remove entries.
   Handles made by the preload before the caps were dropped are recorded by
   walking the globals once when the caps change. The trap to design around:
   a count or index that happens to equal a live handle's address is not a
   forgery, and the kind key is what keeps `(vec-get (vec-len v) 0)` from
   passing.
2. **Tagged handles** (the complete route). A `TURI_HANDLE` value tag carrying
   a kind and the pointer, returned by every constructor and checked by every
   consumer. It closes the class for unrestricted envs too, and it touches
   every one of the 204+ natives and their Turmeric-side callers that treat
   the handle as an `:int`.

Direction 1 is enough to make the T3 promise and the deferred T1 macro promise
in `docs/guides/security-guide.md`.

## Resolved: host exit (2026-09-30)

The report was filed with a second half: `panic`, and the error paths of
several natives (`vec-get` out of bounds, `slice-get`, `sized-buf-*`,
`json/get!`, a failed `tur-contract-check`), called `exit`, `_exit` or `abort`,
so sandboxed text could end the embedding host. A panicking `defmacro*` ended
`tur check` itself.

Now `turi_eval` and `turi_call` on an env without `TURI_CAP_PROC` install a
landing pad (`host_guarded_run` in `src/turi/eval.c`). Every path that would
have ended the process jumps there instead: an uncaught panic, a panic under
`no-unwind`, a double panic, and each native error path, through
`turi_host_exit_guard`. The call then returns `TURI_ERROR "panic: <msg>"` and
the env stays usable. User `catch-unwind` is unaffected, because the pad is
reached only where the process would have ended. Unrestricted envs print and
exit exactly as before.

Pinned by the `host-exit/*` cases in `tests/turi/sandbox-eval.c` and by
`tests/fixtures/errors/macro-panic-is-a-diagnostic`.
