# errors/ecs-defsystem-writes-unauthorized expects a diagnostic current spices HEAD never reaches

**Severity:** low (one `requires.spices` negative fixture; invisible unless
the optional sibling checkout is present)
**Found:** 2026-08-17, when cloning `../turmeric-spices` for the row-types R1
demand survey un-skipped the fixture for the first time in this container.

## Summary

`tests/fixtures/errors/ecs-defsystem-writes-unauthorized/expected.diag`
expects

```
TUR-E0003
unbound symbol 'Vel-write-cap'
```

but against `turmeric-spices` HEAD the program never gets that far:
elaboration of the ecs spice itself fails earlier, inside
`spices/ecs/src/ecs/world.tur` (`no instance binding for associated type
'Storage' at this type`, `defdata: could not resolve constructor field
type`, `no typeclass method found for 'Pos'`), so the write-capability
check whose diagnostic the fixture pins is never reached and the harness
reports `diagnostic mismatch`.

Verified pre-existing at ffbde045 with an unmodified build: the harness
fails the fixture identically with and without the macro-provenance change
that was in flight when this surfaced.  The 2026-08-17 provenance NOTE does
additionally appear in the stderr (`in expansion of macro 'defworld' ...`),
but the mismatch is the two missing expected substrings, not the extra
note.

## Root-cause direction (unverified)

Either the fixture predates an ecs spice refactor (world.tur's
Storage-associated-type machinery looks newer than the fixture's
write-capability vocabulary), or the ecs spice at HEAD needs a newer
turmeric than this branch provides (an associated-type feature gap).
Deciding which requires bisecting turmeric-spices against a fixed turmeric
-- start by checking whether ANY current turmeric branch elaborates
`spices/ecs/src/ecs/world.tur` cleanly; if none does, the drift is in the
spice, and the other `requires.spices` fixtures pass only because they do
not import `ecs/world`.

## Repro

```sh
git clone https://github.com/rjungemann/turmeric-spices/ ../turmeric-spices
TUR_TEST_FILTER='^errors/ecs-defsystem-writes-unauthorized$' bash tests/run.sh
```

Without the sibling checkout the fixture PASS-skips and the suite shows
nothing -- which is why this sat invisible.
