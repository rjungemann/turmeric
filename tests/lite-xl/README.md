# Lite XL integration tests

Sync tests for the Lite XL Turmeric plugin and themes shipped in
`tools/lite-xl/`. Phase 2 of
[`docs/upcoming/turmeric-lite-xl-desktop-plan.md`](../../docs/upcoming/turmeric-lite-xl-desktop-plan.md).

## Files

- `check-palette-sync.py` -- pulls the Monaco `turmeric-light` and
  `turmeric-dark` theme blocks out of `web/main.js` and asserts the
  Lite XL ports under `tools/lite-xl/colors/` carry the same hex codes
  for the five syntax tokens we share (comment / string / number /
  keyword / type). Background and editor-chrome colors are not
  checked -- they intentionally differ because Lite XL's `style` API
  does not expose every Monaco editor slot.

## Run

```sh
python3 tests/lite-xl/check-palette-sync.py
```

Exit codes:

- `0` -- themes in sync
- `1` -- one or more hex codes drifted
- `2` -- a source file is missing

Wire into `tests/run.sh` once Phase 2 has matured; for now it is a
standalone check the editor work runs ad hoc.
