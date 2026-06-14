# rtmidi spice: inline-C uses undeclared `rtmidi_api_t`

**Status:** Reported
**Severity:** Low-medium. Pre-existing build failure in the rtmidi
spice's `__parse-midi-api` / `midi-in-new` helpers when the system's
rtmidi C header is `<rtmidi/rtmidi_c.h>` from upstream rtmidi 6.x (as
shipped by Homebrew). Cleanly reproducible on a fresh clone of
`../turmeric-spices` at `main` -- the S1 callback migration branch
(`migrate-cfn-s1-2026-06-14`) does not affect this file.
**Discovered:** 2026-06-14, while end-to-end building the rtmidi spice
to validate the S1 callback migration.
**Scope:** `../turmeric-spices/spices/rtmidi/src/rtmidi/core.tur:42, 75`.

## Summary

`spices/rtmidi/src/rtmidi/core.tur` uses the identifier `rtmidi_api_t`
inside an inline-C body, but no header it includes declares that name.
Upstream rtmidi 6.x's `rtmidi_c.h` declares the enum as `enum
RtMidiApi`, not `rtmidi_api_t`, and the spice does not `typedef` the
shorter alias.

The result is the C compile failure:

```
spices/rtmidi/src/rtmidi/core.tur -- core.c
error: use of undeclared identifier 'rtmidi_api_t'; did you mean 'rtmidi_api_name'?
   rtmidi_api_t api_val = RTMIDI_API_UNSPECIFIED;
```

at every callsite.

The S1 migration branch is unaffected -- it touches only `rtmidi/in.tur`
-- but `tur build spices/rtmidi` cannot complete without this fix.

## Repro

```sh
brew install rtmidi
CPATH=/opt/homebrew/include LIBRARY_PATH=/opt/homebrew/lib \
  tur build ../turmeric-spices/spices/rtmidi
# → use of undeclared identifier 'rtmidi_api_t'
```

Same failure on a clean `main` checkout (predates the S1 branch).

## Proposed fix

Either:

1. **Typedef the alias inside the inline-C body** (smallest change):

   ```turmeric
   (defn __parse-midi-api [api : cstr] : int
     ```c
     #include <rtmidi/rtmidi_c.h>
     typedef enum RtMidiApi rtmidi_api_t;
     rtmidi_api_t api_val = RTMIDI_API_UNSPECIFIED;
     ...
     ```)
   ```

2. **Rename `rtmidi_api_t` to `enum RtMidiApi`** throughout the two
   functions. Slightly more invasive but reads as the upstream header
   intended.

Either lands as a spice-side patch in `../turmeric-spices` -- no main
compiler change needed.

## Cross-references

- `docs/reported/c-fn-ptr-element-and-size-precision-gap.md` -- the
  c-fn precision gap surfaced alongside this issue while building
  rtmidi end-to-end. Independent of this one but discovered in the
  same session.
- `../turmeric-spices` branch `migrate-cfn-s1-2026-06-14` -- the S1
  callback migration; not the cause of this build failure but its
  end-to-end validation surfaces it.
