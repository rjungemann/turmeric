---
title: The region store-hook set was never swept across the stdlib -- ~25 payload stores in 10 modules carry no note
category: Reported
description: CLAUDE.md names a hooked set, but it was assembled case by case rather than by a sweep. future.tur and fiber.tur are fixed here; threadpool, stm-sync, json, schema, httpd, zipper, arrow, image, args and serial still store an erased caller word into memory that outlives the bracket with no TUR_REGION_NOTE.
---

# The region store-hook set was never swept across the stdlib

**Severity: medium.** Each missing hook is a potential silent use-after-rewind
on the default build -- the failure mode
[region-escape-through-unhooked-stores](region-escape-through-unhooked-stores.md)
documents -- but only for a program that actually stores a region-allocated
node through that particular primitive, so most of these are latent.

**Status:** OPEN for the modules listed below. `future.tur` (4 stores) and
`fiber.tur` (1) are **fixed**, with fixture coverage, in the change that filed
this.

## How this was found

While fixing `future.tur` and `fiber.tur` it was not obvious whether those two
were unlucky or representative, so the question was answered by sweeping rather
than guessed at. The heuristic: for every `stdlib/*.tur` inline-C body with no
`TUR_REGION_NOTE`, find an assignment of one of the enclosing `defn`'s own
parameters into dereferenced memory (`->field = p;` or `arr[i] = p;`).

54 candidates. Most are genuine integers being stored as integers -- a `cap`, a
`threshold`, a `count`, a local `union` in `bits.tur` -- and want no hook. What
remains is roughly **25 stores across 10 modules** where the word is an
**erased payload** the caller handed in, and so could be a node.

## The set that still needs hooks

Signatures confirmed; reachability with a region-allocated node not
individually proven, so treat this as a work list rather than 25 filed bugs.

| Module | Primitive(s) | Note |
| --- | --- | --- |
| `threadpool.tur` | `work-queue-push`, `thread-pool-submit`, `thread-pool-dynamic-submit` | `work-queue-push [q : WorkQueueHandle v : int]` is **exactly `chan-send`'s shape**, and `chan-send` was hooked in the S2 change |
| `stm-sync.tur` | `tchan-cons-append` | channel payload into a cons cell |
| `json.tur` | `json/array-push`, `json/object-put` | JSON child nodes into a malloc'd array |
| `schema.tur` | `schema/field`, `/array`, `/optional`, `/union`, `/transform`, `/always`, `/ap`, `/field-of`, `/fmap`, `/ap-fat`, `sch-vpush-`, `sch-push-err-` | the largest cluster; every one stores a schema handle or a value into a malloc'd cell |
| `httpd.tur` | `httpd-handle`, `httpd-new-pool`, `httpd-new-async-with-limit`, `router-add`, `httpd-register-tls-impl` | **closure** words into long-lived server state; a closure env can carry a node |
| `zipper.tur` | `zipper-new-raw` | `focus` payload |
| `arrow.tur` | `__ac_cell_new`, `__ac_loop_fix_step` | internal cells |
| `image.tur` | `image/hook-registry`, `image/global-registry` | process-lifetime global registries |
| `args.tur` | `args/spec-subcommand`, `args/parse` | spec handles |
| `serial.tur` | `serial-pair-bytes` | `fst` payload |

Explicitly NOT in the list, having been checked and dismissed: `chan-new`'s
`cap`/`head`/`tail`/`count`, `barrier-new`'s `threshold`, `sem-new`'s
`initial`, `bytes-alloc`'s `n`, `json/int`'s `v`, `schema/literal-int`'s `v`,
`range-new` / `float-range-new` bounds, and the local unions in `bits.tur` and
`map.tur`.

## Why the set drifted

[CLAUDE.md](../../CLAUDE.md) lists the hooked set explicitly and says a new
primitive "joins the list in the same change". That rule works for **new**
primitives. Every module above predates the rule, so nothing ever forced a
pass over what already existed -- the set is the accumulation of the sites
someone happened to touch, not a swept set. `chan.tur` is the precedent: it
carried **zero** notes across three ring-buffer stores until the S2 typing work
happened to open the file.

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

Each hook is one line before the store (`TUR_REGION_NOTE(word);`), expands to
`((void)0)` under `TUR_REGIONS=0`, and is semantically inert otherwise -- so
these are individually trivial and collectively a judgement call about which
words are genuinely node-capable. Suggested order: `threadpool` and `stm-sync`
first (queue payloads, the `chan-send` shape that is already known to matter),
then `json`/`schema` (largest cluster, most mechanical), then `httpd` (closure
words), then the rest.

Worth pairing with a **lint** rather than another manual pass: the sweep above
is a 30-line script, and something that mechanical should not depend on a
person opening the file for an unrelated reason.

## See also

- [region-escape-through-unhooked-stores](region-escape-through-unhooked-stores.md)
  -- the failure mode, and the reason the hook set exists.
- `tests/fixtures/region-escape-via-store` -- the fixture every new hook joins;
  it gained the `promise-fulfill` and `future-of` cases in this change.
