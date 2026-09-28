# turmeric-godot: the shim's per-script state is entered from Godot's worker threads with no lock

**Summary:** Godot can call a script instance from threads other than the main
thread -- `WorkerThreadPool` when a node's `process_thread_group` is
Sub-Thread, and the physics thread when physics runs on a separate thread. The
GDExtension's `cb_call` runs every interpreted method through `turi_call` on the
script's single `TuriEnv`, with no lock, and `TuriEnv` is documented as not
thread-safe. Nothing has ever exercised this: every run so far drove scripts
from the main thread only.

**Severity:** Medium, latent. Godot's defaults keep `_process` and
`_physics_process` on the main thread, so a stock project never reaches it. A
project that opts a node into sub-thread processing or threaded physics
reaches it silently. The likely result is memory corruption inside the
interpreter, not a diagnostic.

Split out 2026-09-28 from J7 of
[godot-binding-refresh-plan.md](../archive/godot-binding-refresh-plan.md), so
the open half is not lost when that plan is archived. This is a hazard found by
reading the code. No crash has been seen, because nothing has run this.

## What is already known -- the MIR half is solved

J7 was filed as "MIR context reentrancy under concurrent compile or execution
is unverified". That half is real and already handled in this repo. Do not
investigate it again:

- MIR-gen is not thread-safe, and the tree measured this: "three different
  assertions across five runs of one fixture"
  ([src/jit_engine.c:428](../../src/jit_engine.c)).
- Turmeric does not use `MIR_set_lazy_gen_interface`. It builds the lazy path
  itself as `jit_lazy_gen_locked`
  ([src/jit_engine.c:469](../../src/jit_engine.c)), which holds a process-wide
  mutex. The double-check inside the lock is the load-bearing half
  ([docs/guides/jit-guide.md:280](../guides/jit-guide.md)).

So a Godot host that ever embeds the JIT
([jit-godot-embedding-spike](jit-godot-embedding-spike.md)) gets code
generation that is already serialized. What remains open belongs to
`turmeric-godot`, not MIR. It applies to the interpreter and AOT paths today,
with or without a JIT.

## What is open -- shim-side state

Read at `turmeric-godot` `04dd018`:

| State | Scope | Where |
| --- | --- | --- |
| `TuriEnv` | **one per script**, shared by every node carrying that script | `src/turmeric_script.cpp:86` (`turi_env = turi_env_new()`) |
| `turi_call(env, ...)` in `cb_call` | no lock | `src/turmeric_instance.cpp:142`, `:253` |
| `g_current_instance` | `thread_local` -- right shape | `src/turmeric_instance.cpp:31` |
| Variant arena `g_arena` / `g_str_arena` | `thread_local`, bracketed per call -- right shape | `src/bridge/variant_marshal.cpp:58`, `:65` |
| `g_preload_cache` | process-wide `std::unordered_map`, no lock | `src/bridge/classdb_proxy.cpp:1264` |
| script `exports` / `signals` vectors | per script, cleared and refilled on reload | `src/turmeric_script.h:112-113` |
| AOT cache bookkeeping (`.godot/turmeric-cache`) | filesystem, staged and built on reload | `src/aot/aot_cache.cpp` |

libturi's own contract: "`TuriEnv` is **not** thread-safe. Use one environment
per thread, or protect access with an external lock"
([docs/guides/eval-api.md:441](../guides/eval-api.md)). The shim does neither.
Two nodes that share a script and sit in different sub-thread process groups
enter one env at once.

`g_preload_cache` has a comment that states the assumption outright: "Godot's
main thread is the only Variant thread anyway". That is the premise this report
questions.

Also untested: whether script reload (`_reload`, which calls `turi_env_reset`,
evaluates the prelude and facade, and may stage an AOT build) can run on a
`ResourceLoader.load_threaded_request` loader thread while the same script's
instances are in use elsewhere. `g_reloading_script` is `thread_local`
(`src/turmeric_script.cpp:77`), which suggests someone once thought about this.

## Why it matters

J7 marked this as the question to settle "before the binding runs anything
beyond `_ready`". Sub-thread process groups are the tool Godot 4 gives a game
that wants to use more cores. A scripting language that corrupts memory when
used from one is a trap, because nothing warns about it.

## How to test

- Build one headless Godot project, with the extension and `libturi` built
  under ASan. Put N nodes on one `.tur` script and give each its own
  `process_thread_group = SUB_THREAD` group. Have `_process` do
  allocation-heavy work (`godot-vec2` math, `godot-prop-get`/`-set`) for about
  1000 frames.
- Repeat with `physics/common/run_on_separate_thread = true` and a
  `_physics_process` body.
- Repeat with several `.tur` scripts loaded at once through
  `ResourceLoader.load_threaded_request`.
- **Confirm the calls actually left the main thread.** Log
  `OS.get_thread_caller_id()` against `OS.get_main_thread_id()` inside the
  script. A run that stayed on the main thread passes and proves nothing --
  the same false-green problem as
  [jit-suite-reports-pass-when-the-engine-is-disabled](../archive/jit-suite-reports-pass-when-the-engine-is-disabled.md).

## Fix directions

1. **Refuse clearly (cheapest).** In `cb_call`, detect a call from a thread
   other than the main thread and `push_error` once per script, naming the
   restriction. Then fail the call. Wrong code becomes a loud error, and the
   README can say "main thread only" truthfully.
2. **One recursive lock per script** around `turi_call`, and around `_reload`
   against those calls. This is correct, but it serializes the scripts the
   user wanted to run in parallel. The lock must be recursive, because a
   script method can call into Godot, which can synchronously call back into
   the same script on the same thread (a signal, or `callv`).
3. **One env per (script, thread).** This follows libturi's contract, but
   each env re-evaluates the prelude and facade, and script globals would no
   longer be shared between threads. That changes the semantics, not just the
   cost.
4. Whatever the choice, `g_preload_cache` needs a mutex unless (1) is chosen.

## Related

- [godot-binding-refresh-plan.md](../archive/godot-binding-refresh-plan.md) --
  J7, where this was first filed.
- [jit-godot-embedding-spike](jit-godot-embedding-spike.md) -- the JIT route.
  Its MIR side is covered above.
- [docs/guides/eval-api.md](../guides/eval-api.md) -- "Thread safety".
