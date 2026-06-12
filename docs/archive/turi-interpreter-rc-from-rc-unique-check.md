# RESOLVED: `ref/from-rc` did not enforce rc uniqueness under `--interpret`

**Status:** Fixed 2026-06-12 (`src/turi/eval.c`). Archived resolution
paper-trail for the `rc-unique-violation` holdout -- the final open item in
[turi-pure-turi-silent-miscompiles.md](turi-pure-turi-silent-miscompiles.md),
which is now fully resolved.

## Summary

`(ref/from-rc rc)` converts a shared `rc<T>` into a unique `ref<T>`; it is only
sound when the rc is *unique* (strong_count==1 and no weak refs), because the
returned ref takes exclusive ownership and frees the control block -- any live
alias would dangle. The compiled `tur_ref_from_rc` enforces this and `abort()`s
on violation:

```c
if (cb->strong_count != 1 || cb->weak_count != 0) {
    fprintf(stderr, "ref/from-rc requires unique rc (strong_count==1 and weak_count==0), got strong=%llu weak=%llu\n", ...);
    abort();
}
```

The interpreter's `EX_REF_FROM_RC` did **no** check -- it silently extracted the
inner value -- and `EX_WEAK` was fully transparent, so a live `weak` was
invisible. `rc-unique-violation`:

```turmeric
(defn main [] : int
  (let [rc (rc/of 77)]
    (let [w (weak rc)]          ;; weak alias -> rc is no longer unique
      (ref/from-rc rc)          ;; must panic: strong=1 weak=1
      0))
  0)
```

ran to a clean `0` exit under `--interpret` instead of panicking with a nonzero
exit. **Severity:** High (silent acceptance of an ownership violation), bounded
to `--interpret`.

## Root cause

The interpreter modeled an rc as an `__rc` struct whose `field[0]` pointed at a
single `int64_t` strong counter; there was no weak counter at all. `EX_WEAK`
returned its operand unchanged without recording the alias, and
`EX_REF_FROM_RC` returned `field[1]` unconditionally.

## Fix

1. **`EX_RC_OF`** now allocates a 2-slot control block: `cnt[0]` = strong,
   `cnt[1]` = weak (`cnt[0]=1, cnt[1]=0`). `field[0]` still points at `cnt[0]`,
   so every existing `*cnt` strong-count reader (clone/drop/count) is unchanged.
2. **`EX_WEAK`** stays value-transparent (returns the underlying rc value) but
   bumps `cnt[1]` when its operand is an `__rc`, so a live weak alias is visible.
3. **`EX_REF_FROM_RC`** reads `strong=cnt[0]`, `weak=cnt[1]` and, when
   `strong != 1 || weak != 0`, raises `turi_runtime_panic` with the same
   `"ref/from-rc requires unique rc (strong_count==1 and weak_count==0), got
   strong=%lld weak=%lld"` message (so the fixture's `expected.stderr` substring
   matches and the process exits nonzero). The unique case is unchanged.

## Validation

- `rc-unique-violation` panics with the expected message and a nonzero exit
  under `--interpret`; added to the `tests/run-turi.sh` allowlist.
- Legit unique conversions still pass with no false panic
  (`rc-auto-drop-consumed-by-ref-from-rc`, `rc-ref-conversion`,
  `rc-elision-ref-from-rc-safety`, ...).
- `TUR=./build/tur bash tests/run-turi.sh` -> `982 passed, 0 failed`.
- Full suite (`bash tests/run.sh`, which validates the `expected.stderr`
  substring) -> `1596 passed, 0 failed`.

## Notes / scope

- The interpreter never *decrements* the weak count on scope exit (it has no
  weak destructors). That is sufficient for the uniqueness check at the point of
  `ref/from-rc` and matches every current fixture; a program that drops a weak
  and *then* expects a unique `ref/from-rc` to succeed is not exercised. If such
  a case appears, model weak-drop alongside the existing `EX_RC_DROP` chain.
