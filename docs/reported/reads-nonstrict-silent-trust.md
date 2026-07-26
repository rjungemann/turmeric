# Unproven `#reads` crossing is silently trusted in non-strict mode; `W0372` text is wrong for `#reads`

**Severity:** low (the safety-critical mode -- `--strict-refine` -- is correct; this
is a diagnostic-quality gap in the non-strict mode and a stale message string).

## Summary

Two related diagnostic issues surfaced once `#reads`-refined accessors could
codegen (the entry-contract suppression, `elab_fns.c` `rt_pred_reads_measure`):

1. **Non-strict silent trust.** An *unproven* `#reads` crossing under
   `--enable=refined` **without** `--strict-refine` produces **no diagnostic at
   all** (exit 0), and the crossing is elided (trusted). A pure refinement in the
   same spot falls back to a runtime contract; a `#reads` crossing cannot (the
   predicate is impure -> `TUR-E0375` -> unemittable), so it is silently trusted.
   A user who forgets `--strict-refine` gets no signal that a stateful read was
   accepted on trust rather than proof.

2. **`TUR-W0372` message is inaccurate for `#reads`.** Under `--strict-refine`
   the same crossing is (correctly) a hard error, but the text reads
   "... runtime check kept". For a `#reads` measure **no runtime check is kept**
   -- the contract is suppressed as unemittable. The message should say the
   crossing could not be discharged and there is *no* runtime fallback for an
   impure measure (so it must be proven), not that a check was kept.

## Reproduce

`tests/fixtures/errors/refine-stateful-shadow-despawn/input.tur` (unguarded read):

```sh
TUR=./build-debug/tur
# non-strict: silent, exit 0, crossing trusted
$TUR --enable=refined check errors/refine-stateful-shadow-despawn/input.tur   # (no output)
# strict: hard error, but the string says "runtime check kept"
$TUR --enable=refined --strict-refine check errors/refine-stateful-shadow-despawn/input.tur
#  error [TUR-W0372]: solver returned unknown ... 'get-Pos!' in 'run'; runtime check kept
```

## Root cause (directions)

- The crossing obligation for a `#reads` measure resolves to `unknown` when the
  guard cannot discharge it. In non-strict mode `W0372` is either strict-only or
  suppressed, and the impure crossing has no runtime-contract fallback, so
  nothing is emitted and nothing warns. It should emit a warning in non-strict
  mode ("stateful crossing trusted; not proven -- compile under `--strict-refine`
  to require proof").
- The `W0372` message string ("runtime check kept") is shared with the pure
  path. For a `#reads`/impure measure it should branch to text that reflects "no
  runtime fallback; must be proven".

## Fix directions

Give the `#reads`/impure-measure crossing its own diagnostic arm: a
non-suppressed *warning* in non-strict mode, and a `W0372` message variant that
does not claim a runtime check was kept. Enforcement under `--strict-refine` is
already correct and should be unchanged.
