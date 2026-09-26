---
title: The region store-hook set was never swept across the stdlib -- ~25 payload stores in 10 modules carry no note
category: Reported
description: 29 stdlib stores of an erased caller word carry no region note, verified in emitted C. The common cause is one place -- the implicit typed-node to :int coercion at a call argument is not a hooked site, while the explicit (:: x :int) is -- so the fix is that coercion, not 29 manual notes. Also finds that a fixture case which erases on the way in does not test the store hook at all, and that only TUR_REGION_STATS=1 can see the difference.
---

# The region store-hook set was never swept across the stdlib

**RESOLVED 2026-09-26, by the recommended shape -- the coercion, not 29
notes -- plus two holes the recommendation stood on.**

1. **The implicit erasure is now an ascription.** Where `elab_call.c`
   accepts a typed node (`TY_ADT` / `TY_APP` / `TY_STRUCT`) into an inline-C
   callee's `:int` parameter, the argument is wrapped in the same internal
   `(:: x :int)` an explicit erasure is. That covers every listed sink that
   takes the word as `:int` -- Tier 1's `work-queue-push`,
   `tchan-cons-append`, Tier 3's `json/*`, `schema/*`, `args/*`,
   `serial-pair-bytes`, and every future one -- with no per-site note.
   A `ptr<void>` sink cannot receive a node implicitly at all (TUR-E0001); it
   takes one only through an explicit ascription, which item 2 covers.
   Tier 2's closure words need nothing new: a node a closure captures is
   noted at the env fill, inside the bracket that owns it.
2. **An explicit erasure written as a call argument was not noted either.**
   The premise that "an erasing ascription is itself a hooked site" held for
   the shape the fixture used -- `(:: (Link n acc) :int)` in `build`'s tail
   -- but the direct-call emitter strips an argument's ascriptions before
   emitting it, so `(f (:: node :int))` handed the node over with no note.
   The strip now remembers an erasure it drops and notes the emitted value
   (`region_ascription_erases_node`, emit_expr.c). This is the one place the
   implicit case (1) reaches the callee too.
3. **`(:: node ptr<void>)` and `(:: node any)` were never noted.** The
   erasure predicate also required that the target type reach no node, and
   the walk answers "reaches" for `ptr<void>` and `any` out of ignorance --
   so the two erasures its own comment named were excluded.

Cost, measured: **zero** snapshot changes across the corpus (nothing in it
hands a node to an erased inline-C parameter), the regions fuzz smoke passes
its rewind/retire model, and the `TUR_REGIONS=0` seam is green. The note is
conservative by construction -- a callee that does not retain the word
(`list-length` on a typed list) now costs that generation's rewind, never
correctness, the trade the typed-parameter note already makes. A declared
`#fx{}` is NOT treated as non-retaining: `zipper-new-raw` is `#fx{}` and
stores its `focus`.

Pinned by `tests/fixtures/region-escape-via-erased-argument`, a `hook.sh`
fixture that asserts the `TUR_REGION_STATS` line as well as the values, as
this report asked: an implicit erasure into an inline-C `:int`, an explicit
`(:: node :int)` argument and an explicit `(:: node ptr<void>)` argument all
retire; a control bracket still rewinds (`pushes=4 rewinds=1 retires=3`).
Before the fix, the implicit and the explicit `:int` cases each measured
`rewinds=1 retires=0` on their own and read back `-2387225703656530210`; the
`ptr<void>` case read garbage even after the other two were fixed.

**Severity: medium.** Each missing hook is a silent use-after-rewind on the
default build -- the failure mode
[region-escape-through-unhooked-stores](region-escape-through-unhooked-stores.md)
documents. **Demonstrated, not inferred**: with `promise-fulfill`'s hook
removed, a node stored from inside a bracket makes the generation rewind
(`rewinds=1 retires=0`) while the future still points into it. It bites only
when the node reaches the store WITHOUT an erasing ascription, which narrows
the exposure considerably -- see the section below.

**Status:** OPEN for the modules listed below. `future.tur` (4 stores) and
`fiber.tur` (1) are **fixed**, with fixture coverage, in the change that filed
this.

## How this was found, and how it was then verified

The first pass was a heuristic over source: for every `stdlib/*.tur` inline-C
body with no `TUR_REGION_NOTE`, find an assignment of one of the enclosing
`defn`'s own parameters into dereferenced memory. 54 candidates.

That is only a source grep, and it cannot see the notes the **emitter** adds on
its own (CLAUDE.md: a TYPED node parameter is noted at body entry). So every
surviving candidate was then checked in **emitted C** -- load the module, find
the function's emitted definition, look for the macro inside its body.

**All 29 non-integer candidates are unhooked in the emitted output**, so the
emitter is adding nothing for any of them. Notably that includes
`httpd-new-pool`, `router-add` and `httpd-new-async-with-limit`, whose handler
parameters were given real function types by the S1 typing work -- **typing the
parameter did not produce an automatic note.**

## What actually decides whether a missing hook can bite

This is the part the first version of this report got wrong, and it narrows the
work list considerably.

**An erasing ascription is itself a hooked site.** So a value that reaches one
of these stores through `(:: x :int)` has ALREADY been noted, and the
store-side hook is redundant on that path. Measured on
`tests/fixtures/region-escape-via-store`, which builds its nodes with
`(build n 0)` -- a helper whose recursion goes through `(:: (Link n acc) :int)`:

| Note removed | `TUR_REGION_STATS=1` |
| --- | --- |
| baseline | `rewinds=1 retires=11` |
| `bt-set!` (case 6) | `rewinds=1 retires=11` -- **no change** |
| `vec-push!` (case 1) | `rewinds=2 retires=10` -- detected |

So **case 6 of that fixture does not test `bt-set!`'s hook at all**; it passes
with the hook removed. Case 1 does, because it pushes a typed `(Link n 0)`
directly.

**The gap the store hook actually covers is the IMPLICIT erasure**: a typed
node passed straight into an `:int` parameter, with no `::` anywhere. That
coercion is accepted by the `TY_ADT -> TY_INT` hatch in `elab_call.c`
(the one commented "Phase G0: ADT values are heap-allocated and passed as
int64_t pointers") and emits a plain cast with **no note**. Proven on
`promise-fulfill` before it was hooked:

```
(with-region (fn [] (do (promise-fulfill p (Link n 0)) 1)))

  with the hook:     region-stats: pushes=1 rewinds=0 retires=1
  without the hook:  region-stats: pushes=1 rewinds=1 retires=0   <-- use-after-rewind
```

**`TUR_REGION_STATS=1` is the only instrument that sees this.** Comparing
stdout does not: with the hook removed the fixture still printed every expected
line, because a rewound generation's memory is not necessarily reused before
the read. A hook test that asserts on output is asserting nothing.

## The set that still needs hooks

Signatures confirmed and emitted-C checked; reachability with a
region-allocated node is argued per tier rather than proven per site.

**Tier 1 -- user-supplied payload into long-lived memory.** The word is
whatever the caller passes, so it can be a node, and the target outlives any
bracket. Same shape as `chan-send`, which was only hooked when the S2 typing
work happened to open that file.

| Module | Primitive(s) |
| --- | --- |
| `threadpool.tur` | `work-queue-push [q : WorkQueueHandle v : int]`, `thread-pool-submit` / `thread-pool-dynamic-submit` (`task_arg : ptr<void>`, read on another thread) |
| `stm-sync.tur` | `tchan-cons-append [lst : int v : int]` |
| `zipper.tur` | `zipper-new-raw` (`focus : int`) |

**Tier 2 -- closure words into long-lived state.** A closure env can capture a
node, and the env fill notes the capture against the closure's own generation,
not the server's.

| Module | Primitive(s) |
| --- | --- |
| `httpd.tur` | `httpd-handle` (`handler : int`), `httpd-new-pool`, `httpd-new-async-with-limit`, `router-add`, `httpd-register-tls-impl` (six erased fn words into a global `httpd_tls_ops`) |
| `image.tur` | `image/hook-registry`, `image/global-registry` -- process-lifetime global registries |

**Tier 3 -- module-domain handles, defensive only.** The declared parameter is
`:int`, so the checker permits a node, but every value the module's own API
produces is `malloc`'d by that module rather than region-allocated
(`json/int` mallocs its node; `schema/array` mallocs `s[]`; `ArgSpec` is a
`defopaque :int` over a malloc'd spec). A node can only arrive here if a caller
deliberately passes one through the erased parameter.

| Module | Primitive(s) |
| --- | --- |
| `json.tur` | `json/array-push`, `json/object-put` |
| `schema.tur` | `schema/field`, `/array`, `/optional`, `/union`, `/transform`, `/always`, `/ap`, `/field-of`, `/fmap`, `/ap-fat`, `sch-vpush-`, `sch-push-err-` (12) |
| `args.tur` | `args/spec-subcommand`, `args/parse` |
| `serial.tur` | `serial-pair-bytes` |

Explicitly NOT in any tier, checked and dismissed as genuine integers:
`chan-new`'s `cap`/`head`/`tail`/`count`, `barrier-new`'s `threshold`,
`sem-new`'s `initial`, `bytes-alloc`'s `n`, `json/int`'s `v`,
`schema/literal-int`'s `v`, `range-new` / `float-range-new` bounds, and the
local unions in `bits.tur` and `map.tur`.

## Aside: `fiber-yield` has no behavioural fixture coverage at all

Worth recording, because it is why the `fiber.tur` half of this change is
verified by reading emitted C rather than by a runtime fixture. **No fixture
calls `stdlib/fiber.tur`'s `fiber-yield`.** Every fiber fixture
(`fiber-yield`, `fiber-effect`, `p19-8-fiber-effect-chain`, ...) defines its
own local inline-C wrapper that calls `tur_fiber_block_yield` directly and
never goes through the stdlib entry point.

`tests/fixtures/scheduler-multithread` looks like a counter-example -- its
snapshot moved when the hook was added -- but that is only because it LOADS the
module: its `expected.c` carries `fiber_hyyield`'s forward declaration and
definition and **no call site**. Emitting is not exercising.

So the stdlib primitive is dead as far as the suite is concerned, which is also
a plausible reason its missing hook went unnoticed.

## Fix direction

**Hooking 29 sites by hand is the wrong shape of fix.** The common cause is one
place: the implicit typed-node -> `:int` coercion at a call argument is not a
hooked site, while the explicit `(:: x :int)` is. Hooking the coercion where
`elab_call.c` accepts it would cover all 29 at once, cover every future `:int`
sink for free, and remove the need for anyone to remember the rule -- which is
what actually failed here.

Per-site notes remain the right answer where the store is reached by a route
that coercion does not cover (a runtime function taking the word, as
`fiber-yield` does).

If the manual route is taken anyway, the order that matches the risk is Tier 1,
then Tier 2, then Tier 3 -- and Tier 3 may reasonably be declined, since a node
can only reach those through a caller deliberately erasing one.

**Whichever lands, the test must assert on `TUR_REGION_STATS=1`, not stdout.**
Per the measurement above, the existing fixture's stdout assertions pass with
the hook removed. The two cases added with this report live in their own
fixture, `tests/fixtures/region-escape-via-store-future`, written to the direct
(unerased) form for that reason and checked both ways: `rewinds=0 retires=2`
with the hooks, `rewinds=2 retires=0` without.

They are a separate fixture rather than cases 10 and 11 of the shared one for a
reason worth knowing before anyone merges them back: **`future.tur` pulls in
pthread, which the MIR engine cannot compile.** Folding these cases into
`region-escape-via-store` made that whole fixture fall back to cc under the JIT
job -- reported as `region-escape-via-store (new cc fallback)` against
`tests/jit-fallback-baseline.txt` -- costing an engine-clean region fixture its
JIT coverage to buy two cases. The split keeps the shared fixture on the
engine and lists only the future one in the baseline, alongside the three
`future-*` fixtures already there for the same reason.

That also means a fixture for any Tier 1 or Tier 2 hook above should check what
its module drags in: `threadpool` and `httpd` are both pthread/socket users and
will land in the baseline the same way.

## See also

- [region-escape-through-unhooked-stores](region-escape-through-unhooked-stores.md)
  -- the failure mode, and the reason the hook set exists.
- `tests/fixtures/region-escape-via-store` -- the fixture every new hook joins;
  it gained the `promise-fulfill` and `future-of` cases in this change.
