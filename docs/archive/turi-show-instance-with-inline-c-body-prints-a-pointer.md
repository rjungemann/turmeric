---
title: "A `Show`/`show` instance with an inline-C body prints a heap pointer under `tur interpret`, with no diagnostic"
category: Archive
description: "Every other inline-C shape the tree-walker cannot run fails loudly -- either the simple executor declines and the clean `inline-C not supported in interpreter mode` guard fires, or a matcher claims it and returns the right answer. A typeclass named exactly `Show` with a method named exactly `show` takes neither path: it returns the impl's raw carrier word, so `(println (.show 7.35))` prints 88167088870656 instead of `float:7.35`. Renaming the class OR the method restores the clean error."
---

# `Show`/`show` + inline-C body = a printed pointer, silently

**RESOLVED 2026-09-09.** The report located the wrong bypass: `turi_call_show_named`
is the `println`-on-a-struct route and never ran here. The actual one is in
`interpreter_natives.c`, which registers stdlib's inline-C `Show` instances as
natives under the ELABORATOR'S MANGLED INSTANCE NAMES (`__inst_Show_show_int`,
`__inst_Show_show_float`, ...), and the "keep native override" branch in
`eval.c`'s defn registration kept such a native for ANY impl that landed on
the same key. A user class spelling `Show`/`show` over `float` produces exactly
that key, so its inline-C body was never registered at all -- stdlib's native
ran instead, and stdlib's `show` returns an owned `String` handle (a
`TURI_INT`), which the user's `cstr`-declared method handed to `println` as a
number. That is the whole isolation table: rename the class, the method, or
change the receiver type off stdlib's set and the key no longer collides.

The fix keeps an `__inst_`-keyed native only for an impl whose defining file
is under `stdlib/` (the FILE, not `in_stdlib_load`, which is false for an
explicit `(load "stdlib/typeclass-show.tur")` -- the same signal the
duplicate-instance warning uses). A user impl falls through to its own
closure, whose inline-C body then reaches the simple executor or the clean
diagnostic like any other: the `%.2f` float body is unclaimed and reports
`inline-C not supported in interpreter mode`; the `%lld` int body is CLAIMED
and answers `int:735` -- the report's other legitimate outcome, so the harness
accepts either and rejects a bare integer. Plain-defn natives keep the
documented override-by-name behaviour (the benchmark `head`/`tail` stub
pattern).

Pinned by `tests/run-interp-show-inline-c.sh` (ctest `tur_interp_show_inline_c`):
the four isolation rows, the pure-bodied user `Show`, the compiled path, and
stdlib's own `show-line` on a float under `--interpret` (the override the fix
must keep). A dedicated runner because `run-turi.sh` PASS-skips every inline-C
program. Direction 3 (stop keying on names) is untouched: the natives are
still keyed by mangled name, but now only claim stdlib's bodies. Interpreted
2002/0, compiled 2910/0.

---

**Severity: medium.** A silent wrong answer, but on a path that is documented
as unsupported and that no suite covers (`run-turi.sh` PASS-skips every fixture
containing a user inline-C block -- 768 of them). So it cannot regress CI, and
it cannot produce a wrong answer in a COMPILED program. What makes it worth
filing is the inconsistency: every other inline-C shape the interpreter cannot
run says so, and this one does not.

Found while checking whether S9's dynamic typeclass dispatch got float receivers
right (saffron-lang-plan S9 / D8). It did; the control failed, which is what
turned this up. **It is not a Saffron bug and not an S9 regression** -- it
reproduces in plain Turmeric with no dynamic dispatch anywhere, and predates
that work.

## Repro

```turmeric
(defclass Show [a] (show [x] : cstr))
(definstance Show [float]
  (show [x] : cstr
    ```c
    char *b = (char *)malloc(32);
    snprintf(b, 32, "float:%.2f", x);
    return (const char *)b;
    ```))
(defn main [] : int (println (.show 7.35)) 0)
```

```
$ tur run p.tur          # compiled -- correct
float:7.35

$ tur interpret p.tur    # stdout
88167088870656
$ tur interpret p.tur 2>&1 >/dev/null    # stderr -- nothing
```

`88167088870656` is the address `malloc` returned: the impl ran (or its carrier
word was taken) and the result was handed back as a `TURI_INT` rather than a
`TURI_CSTR`, so `println` printed the pointer.

Use a fractional literal here. `7.0` would print the same pointer and look
equally wrong, but a reader checking a rounding question would then be unable to
tell this defect from a float/int conversion one.

## Isolation -- it is the NAMES, not the body or the type

The same body, varying only the class and method name:

| class | method | receiver | interpreter |
|---|---|---|---|
| `Show` | `show` | float | **88167088870656**, no diagnostic |
| `Show` | `show` | int | **88167088870656**, no diagnostic |
| `P` | `f` | float | clean `inline-C not supported` |
| `Show` | `render` | float | clean `inline-C not supported` |
| `Displayish` | `show` | float | clean `inline-C not supported` |

Renaming EITHER half is enough. So this is not about the body, the receiver
type, or typeclass methods in general.

Two controls, both correct, which bound the defect from the other side:

- The identical body in a plain `defn` -> clean `inline-C not supported`.
- `Show`/`show` with a PURE Turmeric body (`(show [x] : cstr "an-int")`) ->
  prints `an-int`. The `Show` path is fine; it is `Show` + inline-C that breaks.

## What should have happened

The interpreter has two correct outcomes for an inline-C body and this reaches
neither:

1. `try_exec_simple_inline_c` (`src/turi/eval.c:5598`) pattern-matches a handful
   of simple shapes and returns a real answer. Every matcher announces itself
   through `ic_claim`, so `TUR_IC_TRACE=1` logs a line.
2. When no matcher claims it, `eval_expr`'s `EX_INLINE_C` arm returns the
   documented clean carve (`src/turi/eval.c:10308`), `"inline-C not supported in
   interpreter mode"`.

Under `TUR_IC_TRACE=1` the failing case logs **no line at all** and emits **no
error**, so it reaches neither -- the body is never offered to the executor and
never reaches the guard.

For contrast, a `Probe`/`p-int` class with `return x + 100;` logs
`[ic-trace] __inst_Probe_p_hyint_int claimed by simple-return -> tag=2 int=105`
and answers 105 correctly, so the typeclass-method path CAN reach the executor.
Only the `Show`/`show` spelling does not.

## Root cause (partial -- the bypass is located, the exact call is not)

The interpreter carries a name-keyed special path for `Show`:
`turi_call_show_named` (`src/turi/eval.c:14079`) searches the registry for a
typeclass literally named `"Show"`, then for a method literally named
`"show"`, then for the instance matching the receiver's type name, and calls
the impl through a synthesized closure. `TuriEnv` keeps a `last_tc_env` field
solely to feed it (`src/turi/env.h:387`, "used by turi_try_show for Show
dispatch").

That shape -- match on two hardcoded names, then invoke the impl directly --
matches the observed behaviour exactly, including why renaming either name
restores the normal path. What I have **not** confirmed is that this specific
function is the one running for an EXPLICIT `(.show x)` call; it is documented
as the `println`-on-a-struct route, so there may be a sibling doing the same
thing. Confirm with a breakpoint before fixing.

Whichever it is, the bug is the same: it invokes the impl without asking whether
the body is `EX_INLINE_C`, and hands the result back untyped.

## Fix directions

1. **Check the body kind before the synthesized call.** Wherever the `Show`
   fast path invokes the impl, refuse when `impl->body->kind == EX_INLINE_C`
   and fall through to the ordinary application path -- which already offers the
   body to `try_exec_simple_inline_c` and then to the clean guard. Smallest
   change, and it routes both outcomes through the code that already gets them
   right.
2. **Return through the same conversion the ordinary path uses.** The result is
   coming back as a bare carrier word; the ordinary path re-tags an inline-C
   result by the declared return type (`eval.c:9096` and the ADT/struct re-tag
   just below it). Reusing that would make the `cstr` case correct rather than
   merely loud -- but only for bodies a matcher can actually run, so direction 1
   is still needed underneath.
3. **Stop keying on the name.** The deeper issue is that `"Show"`/`"show"` is
   matched as a string, so a user class that happens to use those names gets
   interpreter machinery it never asked for. Out of scope for this fix, but it
   is why the failure is so surprising.

Direction 1 is the honest minimum: it converts a silent wrong answer into the
error the rest of the interpreter already gives.

A test wants all three rows of the isolation table -- `Show`/`show`,
`Show`/`render`, `P`/`f` -- plus the pure-body control, so a fix cannot make
inline-C loud by breaking the pure `Show` path that works today. It cannot be a
`tests/fixtures/` entry as-is: `run-turi.sh` skips anything containing an
inline-C block, which is exactly why this went unnoticed. It belongs in a
harness that asserts the interpreter's DIAGNOSTIC on such a program rather than
its output.

## Not this bug

- **Not float-specific.** An int receiver prints the same pointer.
- **Not the compiled path.** `tur run` prints `float:7.35` in every case above.
- **Not S9 / Saffron.** Every repro here is plain Turmeric with a static
  receiver. S9's dynamic dispatch reaches the same instance and diverges the
  same way, because it calls the same impl -- the pure-Turmeric-bodied dynamic
  dispatch fixture (`saffron-dyn-typeclass-dispatch`) agrees on both back ends.
- **Not "the interpreter should run inline-C".** It is documented as not
  running it, and 768 fixtures are skipped on that basis. The complaint is only
  that this one shape does not SAY so.
