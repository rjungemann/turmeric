# Graduating `--enable=option-niche` makes a legal `Some(NULL)` abort, and no release note says so

**Severity: low today, breaking at graduation.** The niche is flag-gated, so
nothing in the corpus or in user code hits this until the flag defaults on.
Filed 2026-08-30, split out of the option-niche graduation hold (reason 2),
which is currently the only place this break is written down.

**This is not a code defect.** Both aborts are deliberate, correct, and
pinned by fixtures. What is missing is the release-notes entry -- and the
entry is a graduation prerequisite, not a nicety, because the break lands on
code that never opted in to anything.

## Summary

On the default path a carrier `Some(NULL)` is a legal, distinct value:

```turmeric
(defn evil [] : (Option String)
  ```c
  return tur_some_ptr(0);
  ```)
```

`some?` answers **true** on it, it is distinguishable from `(none)` (which is
the null carrier since SR3 slice A), and it round-trips. Under
`--enable=option-niche` the same value is an `abort()` with a diagnostic on
stderr. That is the `:non-null` declaration being enforced -- the niche claims
the bit pattern `0` for `None`, so a null payload would silently read back as
`(none)` -- but a program that declared nothing and asked for nothing would
start aborting the day the flag flips.

## Repro

`tests/fixtures/option-niche-carrier-some-null-aborts` already is the repro;
it is written to assert the abort, so run its input both ways:

```sh
./build/tur run tests/fixtures/option-niche-carrier-some-null-aborts/input.tur
# => some

# the flag is global: it goes BEFORE the subcommand, as tests/run.sh passes it
./build/tur --enable=option-niche run \
    tests/fixtures/option-niche-carrier-some-null-aborts/input.tur
# => tur: a carrier Some with a NULL payload crossed into a niche-represented
#    Option -- the payload type's :non-null declaration was violated
#    (tur_some_ptr(0)?)
#    Aborted
```

## Where the two doors are

Both are intentional and both are the same declaration enforced at a different
crossing:

| door | site | fires on |
|---|---|---|
| construction | `types.c:2062` (the niche `Some` ctor, `emit_registered_adt_app_rec`) | `(some x)` where `x` is 0 -- inline-C or a coercing `::` that the elaborator could not prove |
| carrier -> niche read | `emit_module.c:7584` (`tur_opt_value_checked`) | a `tur_some_ptr(0)` box crossing into a niche consumer |

A *provable* violation -- the literal `0` ascribed in, through any nesting of
relabels -- never reaches either: it is `TUR-E0303` at elaboration
(`ascribe_check_non_null_zero`, elab_types.c).

## Why the existing CHANGELOG entries do not cover it

`CHANGELOG.md` 0.41.0 documents `:non-null`, `TUR-E0303`, and the unshelving
of the niche. It describes the runtime abort only as the fallback for a
computed zero the elaborator cannot prove -- accurate, and not the same
statement. Nothing in any released entry says that **a value which is legal
today stops being representable**, which is the part a user hits.

The break is currently recorded in exactly one place:
`docs/upcoming/sr3-option-niche-plan.md`, "The graduation call", hold reason 2.
A plan under `docs/upcoming/` is not release notes.

## Fix direction

Write the entry, and write it before the flip rather than during it. Draft,
to be placed under `### Changed` (with the **Breaking** lead the
opaque-pointer-c-spelling entry set the precedent for) in whichever release
graduates the experiment:

> - **`(some p)` over a `:non-null` payload can no longer carry NULL.** With
>   the Option niche now default-on, an `(Option P)` for a `:non-null` opaque
>   or a compiler-lowered heap collection is carried AS its payload pointer --
>   16 bytes to 8, `(none)` as NULL, no tag word. The representation spends
>   the bit pattern `0` on `None`, so a `Some` whose payload is null has
>   nowhere left to live.
>
>   **Breaking for inline-C that builds an Option over such a payload.**
>   `tur_some_ptr(0)` used to produce a legal value that `some?` answered true
>   on; it now aborts with a message naming the type and the violated
>   declaration, at construction or at the carrier crossing, whichever comes
>   first. The fix is one line and is almost always what the code meant:
>   return `tur_none()` for the absent case. If a payload type genuinely has a
>   valid null, drop `:non-null` from its `defopaque` -- that un-elects it from
>   the niche and restores the 16-byte tagged form, at no other cost.
>
>   A *provable* violation (the literal `0` ascribed into a `:non-null` handle)
>   has been `TUR-E0303` at elaboration since 0.41.0 and is unaffected.

Two things the entry must not do, both learned from the 0.41.0 opaque-pointer
entry that got this right:

- **Do not describe the abort as a crash.** It is a diagnostic with a message
  naming the type and the declaration; saying so is what makes it actionable.
- **Do not bury the opt-out.** Removing `:non-null` is the escape hatch and it
  is one word, so it belongs in the entry rather than in a plan.

## Related

- `docs/upcoming/sr3-option-niche-plan.md` -- the graduation hold. Of its
  three conditions, the soak (condition 1) has now been quiet across 0.41 and
  0.42; this report is condition 2 and
  `option-niche-container-elements-box-at-parity` is condition 3.
- `tests/fixtures/option-niche-null-payload-aborts` -- the construction door.
- `tests/fixtures/errors/ascribe-zero-into-non-null-opaque` -- the provable form.
