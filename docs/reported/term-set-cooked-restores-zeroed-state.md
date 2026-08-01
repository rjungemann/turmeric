# `term/set-cooked` restores zeroed state, not what `term/set-raw` saved

**Severity: medium (all platforms; silently leaves a terminal in the wrong
mode).** `term/set-cooked` cannot restore the state `term/set-raw` saved,
because the two functions each declare their own function-local `static` with
the same name. Its docstring claims the opposite.

Found 2026-07-31 while porting `stdlib/term.tur` to Windows. **Not
Windows-specific and not introduced by that port** -- the POSIX `termios` path
has the same shape and always has.

## The defect

`stdlib/term.tur`:

```turmeric
(defn term/set-raw [fd : int] : int
  ```c
  static struct termios saved_termios;    ;; <- static local to term/set-raw
  ...
  if (tcgetattr((int)fd, &saved_termios) != 0) return -1;
  ...
  ```)

(defn term/set-cooked [fd : int] : int
  ```c
  static struct termios saved_termios;    ;; <- a DIFFERENT static
  ...
  return (int64_t)tcsetattr((int)fd, TCSAFLUSH, &saved_termios);
  ```)
```

Each `defn` lowers to its own C function, so these are two distinct objects with
static storage duration, not one shared one. `set-cooked` therefore passes a
zero-initialized `struct termios` to `tcsetattr`.

Confirmed in the emitted C (Windows branch shown, POSIX is the same shape):

```sh
./build/tur emit-c tests/fixtures/term-string/input.tur | grep -n saved_console_mode
# 8836:  static DWORD saved_console_mode;      <- in term_slset_hyraw   (starts 8827)
# 8869:  static DWORD saved_console_mode;      <- in term_slset_hycooked (starts 8862)
```

## Consequence

`term/set-cooked` does not restore the previous mode; it *applies a zeroed
one*. On POSIX a zeroed `struct termios` is not "cooked" -- it clears every
`c_iflag`/`c_oflag`/`c_lflag` bit (so no `ICANON`, no `ECHO`, no `OPOST`) and
zeroes the control characters, which is closer to raw than to cooked. A program
that raws the terminal and then calls `set-cooked` leaves the user's shell in a
broken state on exit.

The docstring says "Restores the termios state that was saved by the last
term/set-raw call", which is not achievable as written.

## Why it has not been noticed

`tests/fixtures/term-string` exercises the ANSI/color helpers and size queries,
not the raw/cooked round trip. Nothing in the suite asserts that a terminal's
mode survives `set-raw` -> `set-cooked`, and neither function is reachable from
a non-tty CI run: both bail early (`tcgetpgrp` guard on POSIX, `GetConsoleMode`
failure on Windows) when stdout is redirected, which is how CI always runs them.

## Fix directions

The state has to live somewhere both functions can see. Options:

1. **File-scope state in the runtime.** Cleanest, but inline-C bodies cannot
   declare file-scope objects today -- only `#include`/`#define` are hoisted
   (`tur_hoist_top_includes_scan`). Would need a hoisting mechanism for
   declarations, or a small runtime helper pair (`tur_term_save_mode` /
   `tur_term_restore_mode`) in `src/runtime/`.
2. **Fold both into one inline-C function** taking a flag (`raw?`), with the
   single `static` inside it, and make `term/set-raw`/`term/set-cooked` thin
   Turmeric wrappers. Smallest change that actually works.
3. **Return the saved state to the caller** and take it back -- i.e. make
   `term/set-raw` return an opaque handle that `term/set-cooked` consumes. Best
   typed design (and per CLAUDE.md's no-`:int`-stand-ins rule it should be a
   `defopaque`, not an `:int`), but it changes the public signature of both.

Option 2 is the pragmatic fix; option 3 is the right one if the API is still
free to move.

Whichever is chosen, add a fixture that asserts the round trip on a real pty,
or at minimum that `set-cooked` writes back the same bytes `set-raw` read --
the current suite cannot catch this class at all.
