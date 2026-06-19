---
title: tourist `swap_reject_test` -- `captures-count` sees `expected Captures, got int`; a `Captures` value collapses to its `:int` carrier across some boundary
category: By-value / carrier ABI -- user type loses nominal identity, reads back as `int` carrier (suspected miscompile / typecheck drift)
severity: Medium. Fails the tourist suite at `captures-count` in
  `swap_reject_test.tur`. Symptom is the classic carrier-collapse signature
  (`expected <UserType>, got int`) seen repeatedly during the by-value
  migration; the `Captures` regex handle is being typed as the bare `:int`
  carrier at the call site instead of its nominal type.
status: OPEN
---

# `Captures` collapses to `int` at the `captures-count` call site

## One-line summary

The tourist regex API exposes a `Captures` type (a `defopaque`/`defstruct`
handle over `:int`, defined in the tourist/regex spice). On tip-of-main,
`swap_reject_test.tur` fails type-checking at `captures-count` with
`expected Captures, got int`: the value flowing into `captures-count` -- the
result of a regex match / capture-producing call -- has lost its nominal
`Captures` identity and reads back as the raw `:int` carrier.

This is the same defect family as the archived carrier-collapse reports:

- `docs/archive/defstruct-bare-user-type-field-reads-back-as-int-carrier.md`
- `docs/archive/defopaque-struct-payload-fails-through-unsafe-helper.md`
- `docs/archive/carrier-option-producers-gated-on-handle-typing.md`
- `docs/archive/decode-bool-carrier-instance-ascription.md`

In each, a user type whose ABI is `:int` (opaque-over-int, or an aggregate
carried as int64) is correctly produced but the *consumer* observes the carrier
`int` rather than the nominal type, tripping `TUR-E0001`
(`expected <T>, got int`).

## Likely boundary

`Captures` most plausibly crosses one of these, where carrier->nominal
re-typing is known to be fragile:

1. **`option<Captures>` / `result<Captures, _>` unwrap.** If the match returns
   `option<Captures>` and `swap_reject_test` does `(some-val ...)` /
   `(ok-val ...)`, the unwrap may yield the `:int` carrier instead of
   `Captures` -- cf. `carrier-option-producers-gated-on-handle-typing.md`.
2. **A typeclass-instance method return** declared to produce `Captures` whose
   carrier body returns the int64 handle, with the call site not re-typed --
   cf. `decode-bool-carrier-instance-ascription.md`.
3. **An `#{Unsafe}` inline-C helper** declared `: Captures` whose result is read
   back as `int` -- cf. `defopaque-struct-payload-fails-through-unsafe-helper.md`.

## Could not reproduce locally -- spice not present

The tourist/regex spice lives in `../turmeric-spices/`, which is **not** checked
out in this container, so the exact `Captures` definition and the
`captures-count` call site in `swap_reject_test.tur` could not be inspected or
reduced to a compiler-side fixture. The classification above is by signature
match against the resolved carrier-collapse reports, not a confirmed root cause.

## Fix directions / next steps

1. Reproduce with the spice present:
   `git clone https://github.com/rjungemann/turmeric-spices/ ../turmeric-spices`
   then `tur test` the tourist suite and capture the full `captures-count`
   diagnostic + the `Captures` definition (`defopaque` vs `defstruct`, and the
   shape of the capture-producing call's declared return type).
2. Reduce to a minimal compiler-side fixture: a `defopaque Captures :int`, a
   producer with the relevant return shape (`option<Captures>` /
   instance-method / unsafe helper), and a `(captures-count c : Captures)`
   consumer -- the smallest form that reproduces `expected Captures, got int`.
3. The fix almost certainly lands in the same machinery the archived reports
   touched: `call_wrap_reinterpret` / carrier-bridge re-typing in
   `src/compiler/elab_call.c` and `src/compiler/emit_core.c`
   (`emit_carrier_bridge`). Identify which boundary drops the nominal type and
   re-type the carrier result back to `Captures` there, mirroring the existing
   Option/Result reconstruction fix.
4. Per the float-testing rule, when the capture indices/counts are involved,
   probe with a non-trivial value (a 2+ capture match), not a single/zero-capture
   match that could mask an off-by-one in the count read.

## Note

Like the other two tip-of-main regressions reported alongside this
(`ground-class-dict-singleton-references-dce-dropped-instance-method.md`,
`thread-spawn-fn-not-removed-stdlib-thread-needs-resolution.md`), this surfaced
while verifying spices against latest main after the M7/by-value work. The
carrier-collapse class specifically tends to regress as new producer/consumer
shapes migrate to by-value; a `Captures`-shaped fixture in the compiler suite
would guard this boundary going forward.
