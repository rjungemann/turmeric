# MIR Interpreter Tier Plan (tier-0 `MIR_interp`)

Status: **PROPOSED -- no code.** Depends on the JIT engine (J1/J2/S2, see
[jit-engine-plan.md](jit-engine-plan.md)), which is landed.

This plan opens by retracting the premise that motivated it. Read section 0
before section 1: the headline benefit that made this idea attractive does
not exist, and the plan is worth doing for different, smaller reasons.

## 0. The correction

The originating idea was: MIR ships a bytecode interpreter (`MIR_interp`,
`MIR_set_interp_interface`, `mir.h:638-644`), so a tier that interprets MIR
instead of generating native code would give runtime Turmeric evaluation
with no W^X dance, no `MAP_JIT`, no `com.apple.security.cs.allow-jit`
entitlement -- and would therefore run in places a JIT cannot, iOS above all.

**That premise is false.** The evidence is in the vendored tree:

- `MIR_set_interp_interface` calls `redirect_interface_to_interp`
  (`mir-interp.c:2050`, `:2046`), which is
  `_MIR_redirect_thunk (ctx, func_item->addr, _MIR_get_interp_shim (ctx, func_item, interp))`
  (`mir-interp.c:2047`).
- `_MIR_get_interp_shim` and `_MIR_redirect_thunk` publish **native code**
  through the context code allocator (`mir.h:718-724`).
- The default code allocator maps with `MAP_JIT` and toggles
  `pthread_jit_write_protect_np` on Apple platforms
  (`mir-code-alloc-default.c:40-41`, `:68`).

And the shim is not avoidable in any realistic program. The interpreter
resolves call targets through `item->addr` (`mir-interp.c:221`, `:225`,
`:465`) -- the address the shim installs. `MIR_link` does accept a NULL
`set_interface` (`mir.c:2052` guards the whole interface loop), and
`MIR_interp_arr` enters a function directly from a `MIR_item_t` with no
shim at all, so the **host -> interpreted** direction is genuinely
shim-free. But the moment interpreted code calls another interpreted
function it goes through `addr`. A truly shim-free image is one call-free
function, which is not a language runtime.

The pluggable code allocator (`_MIR_init (alloc, code_alloc)`,
`mir-code-alloc.h`) does not rescue this either. It changes *how*
executable memory is obtained, not *whether* -- and on iOS it cannot be
obtained.

**Conclusion: the MIR interpreter tier has the same entitlement, W^X, and
iOS-exclusion profile as the `MIR_gen` JIT.** It is not an escape hatch.

If the requirement is "evaluate Turmeric where runtime code generation is
forbidden," the answer is unchanged and already shipped: the tree-walking
`turi` interpreter, which generates no code whatsoever and is embeddable
today via `turi_eval` / `turi_eval_file` (`src/turi/eval.h:29-32`), with a
sandboxed env (`turi_env_new_sandboxed`) and existing embed tests
(`tur_eval_basic`, `tur_eval_sandbox`, `embed-peripherals`).

## 1. What the tier is actually worth

With the entitlement claim withdrawn, four benefits survive. All four are
about latency and footprint, not reach.

1. **First-call latency.** `MIR_gen` runs instruction selection and register
   allocation; the interpreter runs neither. For eval-shaped workloads --
   a REPL turn, a hot-reloaded spice, a short script -- the code is called
   once or twice and never becomes hot, so codegen is pure overhead. This
   is the dominant cost in exactly the workload Q2 cares about.
2. **Code memory.** One small fixed shim per function item instead of a
   full compiled body. Matters for a long-lived host that evaluates many
   short fragments and never frees a context.
3. **Backend independence.** The interpreter is architecture-neutral. A
   deployment that only ever interprets does not need `mir-gen-x86_64.c` /
   `mir-gen-aarch64.c` linked in at all, which shrinks the binary and makes
   a new host architecture a no-port rather than a port.
4. **A real tier-0 under the existing lazy gen.** This is the strongest
   reason. J1 already implements serialized lazy generation through
   `_MIR_get_wrapper` / `_MIR_redirect_thunk` with a mutex and a
   double-check on `machine_code` (jit-engine-plan findings 34). That is
   precisely the promotion guard a tiered engine needs. Interpreting at
   tier 0 and promoting to `MIR_gen` on a call-count threshold reuses
   machinery that already exists and is already thread-safe.

Benefit 4 subsumes 1 and 2 without giving up steady-state speed, so the
plan targets tiering rather than a standalone interpret-only mode. An
interpret-only mode falls out as the degenerate policy (threshold =
infinity) and is what benefit 3 needs.

### 1.1 Where it does NOT help

- **`tur build` output.** Unaffected and irrelevant -- AOT binaries do not
  generate code at runtime.
- **macOS `tur jit` throughput.** Findings 20.4 already measured the JIT at
  parity with shelling out to `cc` on an M2. A tier that is slower in
  steady state does not improve that; only the S2 split does.
- **Anything about entitlements or iOS.** See section 0.

## 2. Design

### 2.1 Engine policy on the image API

No new API surface. `tur_jit_compile_image` (`src/jit_engine.h:54`) gains a
policy argument; `tur_jit_execute` gains the same. The policy is a small
struct, not an `:int` mode code -- three named fields:

```c
typedef enum {
    TUR_JIT_TIER_GEN    = 0,  /* today's behavior: lazy MIR_gen */
    TUR_JIT_TIER_INTERP = 1,  /* interpret only, never generate */
    TUR_JIT_TIER_MIXED  = 2,  /* interp tier-0, promote to gen */
} TurJitTier;

typedef struct {
    TurJitTier tier;
    unsigned   promote_after;  /* MIXED only: calls before MIR_gen */
} TurJitPolicy;
```

`TUR_JIT_TIER_GEN` must remain the default so every existing caller keeps
today's semantics on an unchanged code path.

Selection inside the engine is one branch at link time:

| Tier | `MIR_link` set_interface | Notes |
|---|---|---|
| GEN | `MIR_set_lazy_gen_interface`-equivalent (today's serialized wrapper) | unchanged |
| INTERP | `MIR_set_interp_interface` | `MIR_gen_init` never called |
| MIXED | serialized wrapper, tier-0 body = interp | promotion under the existing mutex |

### 2.2 Promotion in MIXED

The existing lazy-gen wrapper already intercepts the first call, takes the
mutex, double-checks `machine_code`, generates, and redirects the thunk.
MIXED changes only what happens when the check fails: instead of generating
immediately, bump a per-item counter and redirect to the interp shim until
the counter crosses `promote_after`, then generate and redirect once more.
The mutex and double-check are unchanged, so the concurrency argument that
J1 already established carries over intact -- this is the reason to build
tiering here rather than as a parallel mechanism.

The counter must live beside the existing per-item generation state, not in
a second registry, or the two redirect paths can race each other.

### 2.3 CLI and gating

Under the **existing** `jit` experiment, not a new one. `tur jit` gains
`--engine=gen|interp|mixed` (default `gen`), legal only when `--enable=jit`
is already on. Rationale: this is a mode of the JIT engine, not a separate
feature, and the `jit` row already carries the lifecycle warning and an
`expires_at` of `0.36.0`. Adding a second `EXPERIMENTS[]` row would split
one feature's lifecycle across two contracts and put a second expiry on the
release-cut skills for no benefit.

If the tier outlives `jit`'s graduation -- that is, if `jit` goes always-on
while tiering is still in flux -- it needs its own row at that point, with
every descriptor field populated per the CLAUDE.md rule. Note that as a
follow-up condition on the `jit` graduation decision, not as a new row now.

### 2.4 The embed payoff (the Q2 answer)

`jit_engine.c` is currently added only to the `tur` executable target
(`src/CMakeLists.txt:434`, `target_sources(tur PRIVATE jit_engine.c)`), so
it is in neither `tur_core` nor `libturi` (`:378`, `:504`). Exposing any
JIT tier to embedders means moving it into `tur_core` behind `TUR_JIT` and
publishing the image API through the embed header.

That move is orthogonal to tiering and can land first or last. It is called
out here because it is the reason the tier was asked for: an embedder that
links `libturi` already has the runtime host-resident (S2), which is the
expensive half of every compile -- the fixed preamble was 76% of engine
time. A tiered engine in that process compiles only the program half and
does not pay codegen for fragments evaluated once.

Two constraints the embed path adds:

- **The fallback ladder changes.** `tur jit`'s step-6 fallback shells out to
  `cc` (TUR-W0070). An embedded host has no toolchain at runtime, so its
  fallback must be the tree-walking interpreter instead. That fallback
  already exists and is already linked -- it is the same `libturi`.
- **The section-0 constraint lands on the host.** An application embedding
  a JIT-enabled `libturi` is itself a JIT application: `allow-jit` when
  notarized, and no iOS. This is the argument for keeping `TUR_JIT` a
  build-time option with the tree-walker as the always-available engine --
  which is the shape the code already has.

## 3. Phases

### I0 -- spike and measure (exit criteria, not deliverable)

Build all three tiers against a handful of existing fixtures and measure.
No emitter changes, no CLI, no gating -- a scratch driver against the
vendored MIR is enough, in the shape of `mir-bin-run.c:355-363`, which
already demonstrates both `MIR_link (ctx, MIR_set_interp_interface, ...)`
and the `MIR_gen` path side by side.

Numbers to produce, per tier, on both x86-64 Linux and arm64 macOS:

- link+prepare latency (the thing tier-0 is supposed to win)
- steady-state run time on a compute-heavy fixture (the thing it loses)
- resident code memory after link
- the crossover call count where MIXED beats both

**Proceed only if tier-0 link latency is a large enough win to matter at
eval granularity.** If the interpreter's link cost is within noise of
`MIR_gen`'s on our corpus, the tier buys nothing that S2 has not already
bought, and this plan should be shelved rather than built. Record the
measurement either way -- a negative result is the useful output.

Also confirm in the spike, because both are load-bearing assumptions above:

- c2mir output interprets correctly at all (the corpus has C the generator
  handles and the interpreter might not exercise identically)
- an interp-only context can be built without `MIR_gen_init`, and whether
  the `mir-gen-<arch>.c` objects can then be dropped from the link
  (benefit 3 depends on this and it is unverified)

### I1 -- engine policy plumbed through the image API

`TurJitPolicy` on `tur_jit_compile_image` / `tur_jit_execute`, the
three-way link branch, `--engine=` on `tur jit` under `--enable=jit`.
GEN stays the default and its code path stays byte-identical.

Exit: the JIT fixture suite passes under `--engine=gen` with no diff
against today, and under `--engine=interp` with whatever subset the spike
showed is viable.

### I2 -- MIXED tiering

Promotion counter beside the existing generation state, under the existing
mutex. `promote_after` tunable, defaulted from I0's crossover measurement.

Exit: MIXED is no slower than GEN on the compute-heavy fixtures and no
slower than INTERP on the eval-shaped ones, and the concurrency fixtures
that cover serialized lazy generation stay green.

### I3 -- parity sweep

`tests/run-jit.sh` gains an engine knob and runs the corpus under each
tier. This is the mechanism that catches a tier-specific miscompile, the
same way J3's sweep caught three latent product bugs the compiling
harnesses never saw -- which is the precedent for treating this as
load-bearing rather than polish.

Exit: interp and mixed sweeps at parity with the gen sweep, or a documented
denylist with a reason per entry.

### I4 -- embed exposure

Move `jit_engine.c` into `tur_core` behind `TUR_JIT`; publish the image API
and policy through the embed header; wire the interpreter fallback in place
of the `cc` fallback for embedded hosts.

Exit: an embed test that evaluates Turmeric through the JIT-backed path and
falls back to the tree-walker when `TUR_JIT` is off, in the shape of the
existing `tests/turi/*.c` embed tests.

I4 is independently valuable and does not depend on I1-I3. If I0 comes back
negative, **land I4 alone** -- embedders get the `MIR_gen` engine, which is
the thing they actually asked for, and the tiering work is dropped.

## 4. Risks

- **I0 comes back negative.** Most likely single outcome. Mitigated by
  ordering: I4 carries the user-visible payoff and does not depend on the
  tier existing.
- **Interpreter/generator divergence.** Two engines over one IR is two sets
  of semantics to keep aligned; a tier-specific miscompile is the exact
  class J3 found three of. I3 is the only mechanism that catches it, which
  is why it is a phase and not a footnote.
- **MIXED races.** Promotion touches the same thunk-redirect path as lazy
  generation. Reusing the existing mutex and double-check is deliberate;
  a second parallel mechanism would be a genuine hazard.
- **Scope creep toward "JIT-made executables."** Out of scope and not
  recommended -- MIR has no object-file writer (only textual `MIR_output*`
  and its own `MIR_write`/`MIR_read` serialization), and the
  `mir-bin-driver.c` shape produces a self-JITing binary that pushes the
  section-0 entitlement constraint onto every end-user executable.

## 5. Recommendation

Run I0. Land I4 regardless of what I0 says. Build I1-I3 only on a positive
I0, and expect the honest outcome to be that S2 already captured most of
the available latency and the tier is not worth its second set of
semantics.
