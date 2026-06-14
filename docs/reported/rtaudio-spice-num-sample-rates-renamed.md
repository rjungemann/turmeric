# rtaudio spice: `device-info-sample-rates` reads renamed/removed field

**Status:** Reported
**Severity:** Low-medium. Pre-existing build failure in the rtaudio
spice's `device-info-sample-rates` helper when the system's rtaudio C
header is `<rtaudio/rtaudio_c.h>` from upstream rtaudio 6.0.1 (as
shipped by Homebrew). Cleanly reproducible on a fresh clone of
`../turmeric-spices` at `main`. Mirrors the rtmidi_api_t finding --
upstream renamed/restructured a public field and the spice's inline-C
still references the old name.
**Discovered:** 2026-06-14, while validating the S2 rtaudio +
opengl bundle migration (`s2-rtaudio-opengl-bundle-2026-06-14` in
`../turmeric-spices`). Independent of the S2 retyping change.
**Scope:** `../turmeric-spices/spices/rtaudio/src/rtaudio/devices.tur` --
the `device-info-sample-rates` inline-C body.

## Summary

The spice's inline-C reads `info.num_sample_rates`, but the upstream
`struct rtaudio_device_info` in `<rtaudio/rtaudio_c.h>` (rtaudio 6.0.1,
brew) declares only a fixed `sample_rates[16]` array -- no separate
length field. The C compiler reports:

```
error: no member named 'num_sample_rates' in 'struct rtaudio_device_info';
       did you mean 'sample_rates'?
   ...info.num_sample_rates...
```

at the inline-C site, with a follow-on `-Wint-conversion` error from
the misindexed iteration. The S1 callback migration and S2
opaque-handle migration don't touch this file's body; the bug
reproduces on a stashed working tree against today's `main`.

## Repro

```sh
brew install rtaudio
CPATH=/opt/homebrew/include LIBRARY_PATH=/opt/homebrew/lib \
  tur build ../turmeric-spices/spices/rtaudio
# → error: no member named 'num_sample_rates' in 'struct rtaudio_device_info'
```

## Proposed fix

`rtaudio_device_info` declares:

```c
struct rtaudio_device_info {
    unsigned int id;
    unsigned int output_channels;
    unsigned int input_channels;
    unsigned int duplex_channels;
    int is_default_output;
    int is_default_input;
    rtaudio_format_t native_formats;
    unsigned int preferred_sample_rate;
    int sample_rates[MAX_SAMPLE_RATES];   /* MAX_SAMPLE_RATES = 16 */
    char name[512];
};
```

Iterate `sample_rates[i]` while `i < MAX_SAMPLE_RATES` AND
`sample_rates[i] != 0` (the array is zero-terminated by convention).
Replacement in `device-info-sample-rates`:

```c
int n = 0;
while (n < MAX_SAMPLE_RATES && info->sample_rates[n] != 0) n++;
/* then build the cons list with `n` entries */
```

Spice-side patch only; no compiler change required.

## Cross-references

- `docs/archive/history/rtmidi-spice-rtmidi-api-t-undeclared.md` --
  same shape of bug in the sibling rtmidi spice (upstream
  renamed/restructured a public name; spice's inline-C still
  references the old one). Fixed in spices branch
  `fix-rtmidi-api-t-typedef-2026-06-14`.
- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the audit
  didn't catch this because it focused on Turmeric-level `:int`
  typing, not on C-API drift inside inline-C bodies.
