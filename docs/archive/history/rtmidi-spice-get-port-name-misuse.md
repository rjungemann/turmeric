# rtmidi spice: `midi-{in,out}-port-name` casts an `int` return value to `char *`

**Status:** Reported
**Severity:** Medium. Real latent bug -- `rtmidi_get_port_name` does NOT
return the port name string; it returns an `int` status code and writes
the name into a caller-supplied buffer. The spice currently casts the
returned int to `char *` and returns that as the port name. The cast
hands the caller a garbage pointer that is *almost certainly invalid*
at runtime; reads will either crash or return arbitrary heap garbage.
Compiles only because clang downgrades int->char* to
`-Wint-to-pointer-cast` instead of an error.
**Discovered:** 2026-06-14, while validating the rtmidi spice
end-to-end after fixing the `rtmidi_api_t` undeclared-identifier issue
(see `rtmidi-spice-rtmidi-api-t-undeclared.md`).
**Scope:** `../turmeric-spices/spices/rtmidi/src/rtmidi/core.tur` --
the inline-C bodies of `midi-in-port-name` and `midi-out-port-name`.

## Summary

Upstream rtmidi C API:

```c
RTMIDIAPI int rtmidi_get_port_name (RtMidiPtr device, unsigned int portNumber,
                                    char * bufOut, int * bufLen);
```

It returns an int (length on success / -1 on failure) and writes the
port name into `bufOut`. The caller must (a) preflight the required
length by passing `bufOut = NULL`, (b) allocate, (c) call again with
the buffer.

The spice currently does:

```c
return (char*)rtmidi_get_port_name((RtMidiPtr)(intptr_t)mi, (unsigned int)i, NULL, &len);
```

casting the integer status code itself to `char *` and returning that.
On macOS arm64 + Homebrew rtmidi 6.0.0 this triggers a clang
`-Wint-to-pointer-cast` warning, not an error, so the spice builds and
exposes a function whose return value will segfault any caller.

## Repro

```sh
brew install rtmidi
CPATH=/opt/homebrew/include LIBRARY_PATH=/opt/homebrew/lib \
  tur build ../turmeric-spices/spices/rtmidi
# → builds with warnings, but any call to (midi-in-port-name h 0) or
#   (midi-out-port-name h 0) returns a garbage cstr.
```

## Proposed fix

The correct two-call sequence inside the inline-C body:

```turmeric
(defn midi-in-port-name [mi : int i : int] : cstr
  ```c
  #include <stdlib.h>
  #include <rtmidi/rtmidi_c.h>
  int len = 0;
  /* First call: discover required buffer size. */
  if (rtmidi_get_port_name((RtMidiPtr)(intptr_t)mi, (unsigned int)i,
                           NULL, &len) < 0) {
    return (char *)"";
  }
  char *buf = (char *)malloc((size_t)len);
  if (!buf) return (char *)"";
  if (rtmidi_get_port_name((RtMidiPtr)(intptr_t)mi, (unsigned int)i,
                           buf, &len) < 0) {
    free(buf);
    return (char *)"";
  }
  return buf;
  ```)
```

Same shape for `midi-out-port-name`. Both leak the returned cstr at the
Turmeric level (the caller never frees it) -- but that's consistent
with the rest of the spice's "cstr returned, caller doesn't free"
convention, and is a separate issue.

## Cross-references

- `docs/reported/rtmidi-spice-rtmidi-api-t-undeclared.md` -- companion
  rtmidi spice issue surfaced in the same session. Independent of this
  one (fix lands in the same file but does not interact).
- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the
  broader rtmidi spice audit. Did not catch this bug because the
  audit focused on type-level findings; this is a misuse-of-C-API bug
  hidden behind the typed Turmeric surface.
