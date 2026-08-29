# A `defn` nested inside another `defn` is accepted, and the outer function returns 0

**Severity: critical** (silently wrong runtime behavior, no diagnostic at any
stage). Found 2026-08-28 getting `turmeric-spices` CI green, against
`tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang), independently of the Linux/gcc box it was found on. `tur check` prints
nothing, the program links, and it exits 0 with the wrong answer.

## Repro

A missing close paren makes the following `defn`s parse as nested definitions
inside the previous function's body.

```turmeric
(defmodule nd (export)
  (defn helper [] : int ```c return 42; ```)

  ;; One paren short: `other` is parsed as a nested definition inside
  ;; `outer`'s body.
  (defn outer [] : int
    (let [x (helper)]
      x)

  (defn other [] : int 7))

  (defn main [] : int (outer)))
```

## Observed

`tur check` prints nothing. The emitted C computes the value into `__t163` and
then throws it away:

```c
static int64_t nd__outer() {
        int64_t __t163;
        {
            int64_t __ps_164 = (nd__helper());
            if (tur_panicking) return ((int64_t)0);
            int64_t x_1368 = __ps_164;
            (void)x_1368;
            __t163 = x_1368;
        }
        return 0;          /* <-- __t163 is never returned */
}
```

The process exits `0`; `main` should have returned `42`.

`nd__other` *is* emitted (it appears twice in the output: forward declaration
and definition), so the nested `defn` is hoisted to module scope -- the nesting
is not what breaks. What breaks is that a `defn` occupying the tail position of
`outer`'s body leaves the emitter with no tail value, and it falls back to
`return 0;` rather than the block temp it just assigned.

## Why this is the worst severity in the batch

The other five defects in this batch fail loudly somewhere -- five of them in
`cc`, one silently but only by dropping code that never ran. This one **runs**,
returns a plausible value (`0` is a successful exit status, a `false`, a
"nothing found"), and does it from a source file whose only visible flaw is a
paren an editor will happily reindent around.

There are three independent failures stacked here, and each one alone would
have caught it:

1. The parser accepts a definition in expression position.
2. The elaborator does not reject a `defn` as a function-body tail.
3. Codegen emits `return 0;` for a function whose body produced a value into a
   live temp.

## Expected

A definition in expression position should be a parse or elaboration error --
that is the fix that catches the real-world cause (a paren slip), and it is the
cheap one. Failing that, codegen must not emit `return 0;` for a function whose
body assigned a result temp; that is the backstop that turns any *other* route
to the same state into a build break instead of a wrong answer.

Both are worth doing. (1)/(2) give the author the right message at the right
line; (3) makes the whole class non-silent.

## Where it bit

`spices/watch/src/watch/watch.tur`: `__watch-next-tree` stopped one paren
short, so `__watch-make-cons` and `watch-drain` became nested definitions. The
function always returned 0, so tree-mode `watch-next` could never return an
event.

Three assertions in `tests/tree_test.tur` had been failing for months with no
indication where to look. The single-file path was unaffected, because it
re-stats the target rather than reading the event buffer -- so the spice looked
mostly-working, which is why nobody pulled the thread.

## Fix direction

Start in the elaborator, not the parser: the parser has no reason to know that
`defn` is disallowed in expression position, but the elaborator already walks
the body and knows the context. Reject `EX_*` forms carrying a definition head
when they are not a direct child of a module/top-level, and point the
diagnostic at the `defn` keyword -- "a definition cannot appear here; check for
a missing close paren on the enclosing `defn`" is the message that would have
saved this one, because the missing paren is always the cause in practice.

Then, separately, audit the `return 0;` fallback. Grep the function-body tail
emitter for the path that produces a bare `return 0;` when a result temp was
declared; that pairing (temp declared, temp assigned, temp not returned) is
mechanically detectable and should be an internal compiler assertion, not a
silent zero.

## Guides to update when fixed

- docs/guides/syntax-guide.md -- it does not state that definitions are
  top-level-only.
