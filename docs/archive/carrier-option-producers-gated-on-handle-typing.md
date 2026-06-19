# Carrier-`Option` producers still in stdlib: which migrate cleanly, which are gated

> **RESOLVED 2026-06-19.** End-to-end monomorphization is complete. The small
> residual ABI bridge that remains is intentional and necessary, so the
> carrier-`Option` producers still routed through it (seq step protocol, json,
> safe, serial) are accepted rather than a No-Lazy-`:int` defect to retire. As
> this report already notes, there is no miscompile -- the carrier ABI is
> correct. No further migration work remains here. Archived to docs/archive/.

**Summary.** Beyond the `unwrap-or` cascade (zipper, kleisli), six stdlib
modules still hand-build a carrier `Option` in inline-C (the
`struct { bool is_some; int64_t value; }` malloc pattern) and return it as a
bare `:int`/`:ptr`. Two migrate cleanly to by-value `(Option T)`; four are
gated on prior work and are *not* part of the 1.3 `unwrap-or` cascade.

**Severity.** Ergonomics / type-hygiene (the "No Lazy `:int` Stand-Ins" rule).
No miscompile -- the carrier ABI is correct -- but the surface types erase the
real `Option`/handle structure.

## Migrated (clean, landed)

- `stdlib/zipper.tur` -- all ops, via `-raw` + by-value wrappers; `Zipper`
  retyped `defstruct` -> `defopaque [A] :int` (see the end-to-end
  monomorphization plan, Phase 1.3-step-2).
- `stdlib/env.tur` `env/get` -> `(Option cstr)` (`env/get-raw : int` helper +
  by-value wrapper). No consumers peeked the carrier; zero snapshot churn
  (env is load-on-demand, not auto-loaded).

## Gated -- do NOT migrate piecemeal

### `stdlib/safe.tur` `array-get` -- gated on prelude load order

`array-get` should return `(Option int)`, but `safe.tur` is auto-loaded at
**position 2** (`src/main.c:658`, right after `macros.tur`), *before*
`option.tur` (loaded after `map`/`vec`/`slice`, `src/main.c:705`). Inside
`(defmodule tur/safe ...)` the type constructor `Option` is therefore not yet
in scope, so `(Option int)` fails kind-check (`TUR-E0012`: applies a kind-`*`
type as a constructor). `(load "stdlib/option.tur")` inside `safe.tur` is not a
fix -- `load` is not idempotent for `defstruct`, so it double-defines `Option`
(`'Option' is already defined`). Real fix: move `safe.tur` after `option.tur`
in the prelude order, or make the prelude loader idempotent for already-loaded
modules. Both are prelude-wide changes with their own snapshot-churn /
regression surface; out of scope for a producer retype.

### `stdlib/json.tur` `json/get` -- gated on a `JsonNode` handle type

`json/get` returns a carrier `Option` of a JSON node, but the entire json
surface (`json/null`, `json/bool`, `json/int`, `json/array-new`,
`json/array-get`, ...) types the node as a bare `:int` handle. Migrating
`json/get` to `(Option JsonNode)` is meaningless until the node itself is a
real type (`defopaque JsonNode :int`). That handle-typing is the prerequisite
project; the `Option` retype is a trivial follow-on once it lands.

### `stdlib/seq/*` `seq-make-some` / `seq-make-none` -- gated on a typed step protocol

These are the lazy-seq library's *own* internal `Option` ABI, threaded as
bare `:int` through every combinator (`seq/first`, `seq/nth`, `seq/find`,
`seq/range`'s step fn, ...). They are not isolated producers feeding
`unwrap-or`; they are the seq step protocol. Migrating them requires typing
the whole step protocol (a `(Step A)` / `(Option (Pair A Seq))` shape), a
distinct design effort, not a producer retype.

### `stdlib/serial.tur` -- gated on serialization-internal shape

The two carrier-`Option` mallocs sit inside the (de)serialization paths
(`serial-pair-bytes`, the `cont-*` file ops), where the payload is a raw byte
buffer / pointer, not a clean `(Option T)` at a stable element type. Needs the
`Bytes`/handle types threaded through first.

## Validation of the clean migrations

`bash tests/run.sh` -- 1682 passed; only the pre-existing stale-ECS-spices
`errors/ecs-defsystem-writes-unauthorized` fixture fails (tracked separately in
`docs/reported/ecs-defsystem-writes-unauthorized-stale-diag.md`).
