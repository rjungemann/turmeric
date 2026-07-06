# turi_full_prelude gate is dead: json/schema are now always preloaded

**Status: RESOLVED** (retired the vestigial flag; see Resolution below).

**Summary:** `tests/run-turi-full-prelude.sh` asserts `TUR_TURI_FULL_PRELUDE`
toggles a *carved* module's availability, probing `schema/alt`. But the
interpreter now preloads `json.tur` and `schema.tur` **unconditionally**, so
`schema/alt` resolves with the flag OFF and the gate can no longer observe the
flag. **Severity: low** (stale gate over a graduated feature; no user-facing
miscompile).

## Repro

```sh
cat > /tmp/p.tur <<'EOF'
(defn main [] : int
  (let [s (schema/alt (schema/int) (schema/str))] 0))
EOF
./build/tur --interpret /tmp/p.tur ; echo "rc=$?"   # expected rc=1 (unbound), actual rc=0
```

The test's two failing directions:
- "flag OFF" expects `schema/alt` unbound (rc=1) -- it resolves (rc=0).
- "`=yes` (junk) must not enable" -- it is already enabled regardless.

## Root cause

`src/main.c` (interpreter setup, cmd_eval path):

- The opt-in loader (`full_extra[] = {"json.tur", "schema.tur"}`, ~line 5642)
  only loads them when `TUR_TURI_FULL_PRELUDE=1`.
- But the later JR0/RD reader-macro blocks (~lines 5664-5683) load `json.tur`
  and `schema.tur` **when `!turi_full_prelude_enabled()`** -- i.e. exactly when
  the flag is OFF -- so the `#json(...)` / `#json-str<T>(...)` reader lowerings
  resolve. Net effect: schema/json are loaded in *both* branches, so the flag
  is a no-op for their availability.

`docs/artifacts/turi-preload-carve-out.txt` still lists `json.tur`/`schema.tur`
as carved, which is now stale for the interpreter's runtime availability (they
remain "carved" only in the sense of the `-X` reader gates).

## Fix directions

Pick one, as a maintainer intent call:

1. Retire the gate: `schema/json` have effectively graduated to always-loaded
   under `--interpret`; delete the flag + `full_extra[]` + this test, and drop
   the two entries from the carve-out artifact.
2. Keep the flag meaningful: move the JR0/RD unconditional loads behind the
   same flag (regresses reader-macro resolution when OFF -- probably undesired).
3. Re-point the probe at a module that is *genuinely* still flag-gated (none
   currently is, so this needs a new carve-out first).

Option 1 matches the observed direction of travel (contract/mutmap already
graduated into the default prelude per the code comment).

## Resolution

Took option 1 -- retired the vestigial flag. Since the JR0/RD reader-macro
auto-load blocks (`cmd_eval_h`, `src/main.c`) already load `json.tur` +
`schema.tur` unconditionally under `--interpret`, the `TUR_TURI_FULL_PRELUDE=1`
opt-in loaded them a redundant second way and observably did nothing.

- `src/main.c`: removed `turi_full_prelude_enabled()` and the `full_extra[]`
  opt-in block; the JR0/RD blocks now load json/schema unconditionally (their
  previous `if (!turi_full_prelude_enabled())` guards are gone).
- `tests/run-turi-full-prelude.sh` + its `CMakeLists.txt` `add_test` were
  removed -- the gate cannot hold (no module is flag-sensitive, because none is
  flag-gated anymore).
- `docs/artifacts/turi-preload-carve-out.txt` + `docs/guides/turi-parity-guide.md`
  updated: json/schema stay documented as gaps *relative to the static
  `prelude[]`* the parity ratchet tracks, with a note that they are in fact
  preloaded unconditionally via JR0/RD (their names resolve under `--interpret`).

Verified: `schema/alt` still resolves under `--interpret` (rc=0);
`TUR_TURI_FULL_PRELUDE=1` is now a no-op; `check_turi_native_parity.py` stays
green (json/schema still absent from `prelude[]`); `tests/run-turi.sh` unchanged
at 1426 passed (the 3 remaining `van-laarhoven-*-consumer-*` fixtures are a
separate report).
