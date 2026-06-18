# `errors/ecs-defsystem-writes-unauthorized` diagnostic out of sync with spices `main`

**Severity:** Ergonomics / test-sync (not a miscompile). A `requires.spices`
error fixture pins an expected diagnostic that the current `turmeric-spices`
`main` no longer produces, so the fixture FAILs whenever the spices checkout is
present.

**Discovered while:** fixing the constrained-generic struct-receiver codegen
bug (docs/archive/constrained-generic-struct-receiver-by-ptr.md). Surfaced by a
full `bash tests/run.sh` with `../turmeric-spices` checked out.

## Observed vs. expected

`tests/fixtures/errors/ecs-defsystem-writes-unauthorized/expected.diag` expects:

```
unbound symbol 'Vel-write-cap'
```

Actual stderr (spices on `main`, commit c3f9bcc):

```
.../turmeric-spices/spices/ecs/src/ecs/world.tur:170:24: error:
  no instance binding for associated type 'Storage' at this type
  170 |     `(~(first comps) : (Storage ~(first comps))
```

The ecs spice's `world.tur` now fails to elaborate at the `Storage` associated
type during `world-fields` macro expansion, *before* the fixture's intended
`Vel-write-cap` authorization check is ever reached.

## Why it is not the constrained-generic fix

This is an elaboration-time diagnostic in spices code; the codegen change in the
companion archive report only runs after successful elaboration. The failure
reproduces with the spices checkout on both its feature branch and `main`, and
with the pre-change compiler shape (an error fixture never reaches emit).

## Probable root cause (needs confirmation)

Either (a) the ecs spice's associated-type (`Storage`) machinery regressed and
`world.tur` genuinely no longer elaborates, or (b) the associated-types support
in turmeric changed such that the ecs spice needs updating. The earlier-firing
`Storage` error masks the authorization diagnostic the fixture is meant to test.

## Proposed directions

1. Reproduce `tur check ../turmeric-spices/spices/ecs/src/ecs/world.tur` in
   isolation and bisect whether the `Storage` associated-type binding broke on
   the turmeric side or the spices side.
2. If spices-side: update the ecs spice; if turmeric-side: fix the associated
   type instance-binding resolution.
3. Re-sync the fixture's `expected.diag` only after the masking error is gone --
   do **not** just rewrite the fixture to expect the `Storage` error, since that
   would dodge the real breakage (the authorization check is no longer exercised).
