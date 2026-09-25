# `#lang r7rs`: `char-ready?` and `u8-ready?` always answer `#t`

**Severity:** low. Both back ends. A program that polls a port before
reading, to avoid blocking on a terminal or a pipe, gets `#t` and then
blocks in the read. R7RS 6.13.2: `char-ready?` returns `#t` if a character
is ready on the port, and if the port is at end of file; the point of the
procedure is that a `#t` means `read-char` will not hang. Documented in
docs/guides/r7rs-guide.md ("Where it differs") and r7rs-lang-plan 9.3.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(write (char-ready? (open-input-string "")))            ; #t is right: at eof
(write (u8-ready? (open-input-bytevector (bytevector)))) ; #t is right: at eof
(write (char-ready?))                                   ; #t whatever stdin holds
(newline)
```

```
$ tur run ready.tur            # and tur --interpret; stdin not read
#t#t#t
```

The third answer is the wrong one: nothing is ready on the console, and a
`(read-char)` after it blocks. chibi's two tests (a string port holding
"42", a bytevector port holding one byte) want `#t` and pass here by
construction.

Measured 2026-09-25 against `./build/tur` v0.51.0 (Debug).

## Root cause

stdlib/r7rs/prelude.tur:2842-2845: both procedures resolve the port (so a
closed or output port is still an error) and return `true`. The comment says
why: a read never blocks forever on a string, bytevector or file port, and
at a terminal R7RS leaves the answer open. It does not leave it open -- it
asks for `#t` only when a read would not block -- and a pipe or socket
behind `current-input-port` blocks the same way a terminal does.

The port's buffer (`r7rs_io`, prelude.tur:2505-2519) fills from its `FILE`
with a blocking `fgetc`, so there is no non-blocking primitive to ask.

## Fix directions

- A string or bytevector port is always ready: keep `#t`.
- A file port: `#t` when the buffer holds a byte, or `feof`; otherwise a
  non-blocking check on the descriptor -- `poll()` with a zero timeout on
  POSIX (`fileno(f)`), `PeekNamedPipe` / `WaitForSingleObject` on the
  Windows console and pipes -- behind an inline-C helper the prelude calls
  from both procedures. The interpreter twin registers the same C.
- `char-ready?` on a UTF-8 port strictly needs a whole character; one byte
  ready is the honest approximation every implementation uses.
