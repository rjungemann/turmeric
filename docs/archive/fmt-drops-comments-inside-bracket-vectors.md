# `tur fmt` silently deletes every comment inside a `[...]` vector (and is non-idempotent as a result)

**RESOLVED 2026-08-26.** See "Resolution" at the bottom. The fix went wider
than fix direction (1)+(2): the guard was hoisted into the measure (fix
direction 3), gap re-emission was added to every collection printer plus
`fmt_call` and `fmt_cond`, and trailing comments now keep their own line
instead of being relocated. A **residue in other printers survives** and is
filed as
[fmt-drops-comments-in-handle-and-binding-modifier-gaps](../reported/fmt-drops-comments-in-handle-and-binding-modifier-gaps.md).

**Severity: HIGH** -- silent, unrecoverable source data loss in a
format-in-place tool. `tur fmt` deletes `;`, `;;`, and `;;;` comments that sit
inside a `defstruct` field vector, a `defn`/`fn` parameter vector, or a
`let`/`loop` binding vector. The comments are not moved or reflowed; they are
gone from the rewritten file. `tur fmt` writes in place by default, so on an
unstaged file the text is destroyed with no diagnostic and exit code 0.

A second, visible symptom falls out of the same root cause: `tur fmt` is
**not idempotent** on any file containing such a comment, so `tur fmt` followed
by `tur fmt --check` exits 1 on a file that was just formatted.

## Minimal repro

```turmeric
(defstruct P
  [a : int    ; first field
   b : int    ; second field
   c : int])  ; third field

(defn f [x : int   ; the input
         y : int] : int
  (let [s (+ x y)  ; the sum
        t 2]
    (* s t)))
```

`tur fmt --stdout` (pass 1):

```turmeric
(defstruct P
  [a : int b : int c : int])
; third field

(defn f [x : int y : int] : int
  (let [s (+ x y)
        t 2]
    (* s t)))
```

**Four of the five comments are gone.** The only survivor, `; third field`, sat
*outside* the `]`, and it is relocated onto its own line below the form, where
it now reads as a comment about whatever follows.

`;;;` doc comments are eaten by the same path -- so this does destroy
docstrings, though not module docstrings (see "What this is not", below):

```turmeric
(defstruct Rec
  [;;; the name field
   name : cstr
   ;;; the age field
   age : int])
```
=>
```turmeric
(defstruct Rec
  [name : cstr age : int])
```

### Non-idempotence

Running the formatter on its own pass-1 output collapses the `defstruct`
further, because the comments that forced the break no longer exist:

```turmeric
(defstruct P [a : int b : int c : int])   ; pass 2 -- differs from pass 1
```

Pass 3 == pass 2 (it converges after one extra round), but the fixed-point
violation is enough to break the obvious CI shape:

```console
$ tur fmt file.tur          # exit 0, writes the file
$ tur fmt --check file.tur  # exit 1  <-- on the file fmt just wrote
```

### Found in tree

A sweep of `stdlib/`, `tests/fixtures/`, `examples/`, and `tutorials/`
(223 files: every `.tur.sweet` plus every `.tur` containing a `;;;` line,
two-pass fmt each) found exactly two non-idempotent files, both from this bug:

- `examples/guestbook/src/store.tur` -- loses the three field comments on
  `GuestEntry` (`; HTML-escaped author name`, `; HTML-escaped message text`,
  and relocates `; Unix timestamp (seconds since epoch)` outside the form).
- `examples/guestbook/src/conts.tur` -- same shape on `StoredCont`.

The corpus is otherwise clean, which is why existing coverage misses this --
see below.

## Root cause

`fmt_list` already has exactly the right guard, and its comment explains the
hazard in as many words (`src/compiler/fmt.c:999`):

```c
static void fmt_list(FmtState *s, const Form *f) {
    /* Always try inline first -- but never collapse a form whose source span
     * contains a comment, since the flat printer has no way to re-emit it and
     * the comment would be silently dropped. */
    uint32_t w = fmt_measure(f);
    if (w != UINT32_MAX && s->col + w <= s->opts.line_width
        && !span_has_comment(s, f)) {
        fmt_emit_inline(s, f);
```

**The guard is only on the `F_LIST` path.** The `F_VEC` paths call
`fmt_emit_inline` directly, with no `span_has_comment` check:

- `fmt_param_vec` -- `src/compiler/fmt.c:487-494` (defn/fn parameter vectors)
- `fmt_let` binding branch -- `src/compiler/fmt.c:651-659` (let/loop bindings)

And the *broken* (multi-line) vector printers are no better: unlike
`fmt_body_forms`, which re-emits gap comments via `emit_comments_indented`
(`src/compiler/fmt.c:421`), neither `fmt_vec_params_broken`
(`src/compiler/fmt.c:466`) nor `fmt_vec_let_bindings_broken`
(`src/compiler/fmt.c:602`) ever consults the source gaps -- they walk elements
with `fmt_form` and drop whatever sat between them. That is why the `let`
comment (`; the sum`) is lost even though that vector *was* broken pair-per-line.

So both vector paths lose comments: inline because the guard is missing, broken
because there is no re-emission.

The non-idempotence is downstream of the loss. In pass 1, `span_has_comment` on
the whole `(defstruct ...)` span is true, so `fmt_list` declines to inline and
the form breaks across two lines -- but the vector inside it is then emitted
inline, dropping the interior comments. In pass 2 the span no longer contains a
comment, the guard no longer fires, and the whole form collapses to one line.

## Why existing coverage misses it

`tests/run-fmt.sh` FT8 (`fmt-idempotence-stdlib`) checks `fmt(fmt(x)) == fmt(x)`
over a sample of `stdlib/*.tur`, and there is a comment-preservation check at
`tests/run-fmt.sh:222`. Neither catches this because no sampled stdlib file puts
a comment inside a bracket vector -- the corpus sweep above confirms the only
in-tree instances are the two `examples/guestbook/` files, which no fmt harness
covers. A regression fixture needs a comment *inside* `[...]`, specifically.

## Fix directions

1. Add the missing guard to both vector inline paths -- `fmt_param_vec`
   (`fmt.c:487`) and the `fmt_let` binding branch (`fmt.c:651`) -- so a vector
   whose span contains a comment is never `fmt_emit_inline`d. This alone stops
   the destruction on the inline path and makes the formatter idempotent.
2. Teach `fmt_vec_params_broken` and `fmt_vec_let_bindings_broken` to re-emit
   gap comments the way `fmt_body_forms` does (`emit_comments_indented` between
   consecutive elements), so the broken path preserves them rather than merely
   avoiding the collapse. Without this, (1) converts silent deletion into
   silent deletion at a different line width.
3. Consider hoisting the guard into `fmt_measure` (return `UINT32_MAX` for any
   form whose span has a comment), the same trick used to fix
   `fmt-let-multi-pair-bindings-joined-one-line`. That makes the form
   unmeasurable so no *enclosing* inline check can flatten over it either, which
   is the more robust version of (1) -- a comment inside a vector nested in a
   form that itself fits would still be lost under the narrow fix.
4. Add `tests/run-fmt.sh` cases with comments inside a `defstruct` field vector,
   a `defn` param vector, and a 2+-pair `let` binding vector, asserting both
   preservation and idempotence.
5. Once fixed, reformat `examples/guestbook/src/{store,conts}.tur` -- their
   comments are still intact in git, so the fix should be verified by confirming
   `tur fmt` leaves them that way.

## What this is not

This report was filed after investigating the claim that "`tur fmt`'s own
output isn't idempotent **and eats the module docstring**." The idempotence half
is real and is the same bug as the comment loss. **The module-docstring half
does not reproduce.** A leading `;;;` block survives `tur fmt` in every position
tested (with a `;;` separator, with a blank-line separator, with no separator,
and in a file with no definitions at all), and a `gendocs.py` run over a file
before and after `tur fmt` produces byte-identical HTML -- the module docstring
is still classified as a module docstring. The zero-loss result also held across
all 223 files in the corpus sweep (`;;;` line counts identical pre/post format).
`tur fmt` does normalize away a blank line between the docstring and the first
definition, but that does not change gendocs' classification.

The `;;;`-eating that *is* real is the vector case shown above, which is a
different thing: a doc comment inside `[...]`, not the module docstring.

## Adjacent, not filed here

`tur fmt --stdout tests/fixtures/pkg-sweet-manifest/build.tur.sweet` fails to
parse (`error: unexpected character '#' (0x23)` at 1:2) -- the formatter does
not handle a `#lang` line in a `.tur.sweet` manifest. It was the one skip in the
corpus sweep. Filed separately if it turns out not to be intentional.

---

## Resolution (2026-08-26)

Fixed in `src/compiler/fmt.c`. The corpus sweep this report describes was
re-run before and after, over 2997 files (`stdlib/`, `tests/fixtures/`,
`examples/`, `tutorials/`), two-pass fmt each:

| | files losing comments | files non-idempotent |
| --- | --- | --- |
| before | 70 | 9 |
| after | 8 | 7 |

Every file still failing after the fix was already failing before it -- the
change introduced no new loss and no new non-idempotence. The eight survivors
are a different defect in different printers, filed separately (link above).

### What changed

**1. The guard moved into the measure (fix direction 3, not 1).** A new
`fmt_measure_src(s, f)` returns `UINT32_MAX` for any form whose source span
carries a comment, and every inline-vs-break decision was switched to it:
`fmt_list`, `fmt_param_vec`, the `fmt_let` bindings branch, `fmt_defpackage`'s
map value, and the `F_VEC` / `F_MAP` / `F_SET` / `F_MAP_LITERAL` arms of
`fmt_form`. Doing it in the measure rather than at each call site is what the
report's option (3) predicted: the form is unmeasurable to *enclosing* inline
checks too, so a comment inside a vector nested in a form that would itself fit
cannot be flattened away one level up.

Switching those arms surfaced a latent bug worth naming: they tested
`s->col + w <= line_width` with no `w != UINT32_MAX` check. That is uint32
arithmetic, so an unmeasurable form wraps to a tiny number at any column > 0
and gets inlined anyway -- which would have made the new guard a no-op
everywhere except column 0. Each of those sites now checks the sentinel first.

**2. Gap re-emission in every collection printer** (fix direction 2), via three
new helpers -- `collection_body_start` (offset one past the opening delimiter,
so a comment between `[` and the first element is inside the scanned gap),
`emit_gap_comments_before`, and `emit_trailing_gap_comments`. Applied to
`fmt_vec_params_broken`, `fmt_vec_let_bindings_broken`, `fmt_vec_broken`,
`fmt_map_broken`, and `fmt_set_broken`.

**3. `fmt_call` and `fmt_cond`,** which the report did not cover but which lose
comments by exactly the same mechanism -- a child loop that never consults the
source gaps. `fmt_call`'s head/first-arg pair is where a `#;datum` comment on
the first argument lived (`(println #;99 "ok")` reformatted to
`(println "ok")`); `fmt_cond`'s arm loop is where an arm's trailing `;; note`
lived. Together these two account for 50 of the 62 files the fix recovered.

### Two things found while fixing it that the report did not have

**A trailing comment must not be relocated.** The narrow fix (break instead of
inline, re-emit each comment on its own line) preserves the bytes but moves
`; first field` from the end of field `a`'s line to the line above field `b` --
where it now reads as a comment about `b`. That is a different claim about the
code than the author made, and it is the same criticism the report levels at
`; third field`. `emit_comments_indented` now tracks whether the scan has
crossed a newline yet: a `;` comment that shared a source line with the
preceding element is re-emitted as a trailing comment on the current output
line, and only comments that had their own line get their own line. The same
rule was added to the top-level `emit_comments_in_gap`, which is what fixes
the `; third field` relocation the report's repro shows.

**A comment before a closing bracket can eat the bracket.** Re-emission ends
output *inside* a `;` comment. A collection printer that writes `]` straight
afterwards puts the bracket in the comment, so the reformatted file no longer
parses -- upgrading silent data loss to a syntax error. `emit_trailing_gap_comments`
breaks the line unconditionally when it emits anything, and there is a
regression test (`fmt-comment-before-close-bracket`) asserting it.

A related distinction the helper contract had to carry: a `;` or `#| |#`
comment forces the caller to break, but a single-line `#;datum` is
self-delimiting and stays inline. The gap helpers therefore return "you must
start a new line", not "something was emitted" -- which is what keeps
`(println #;99 "ok")` on one line instead of splitting it across three.

### Layout change: `fmt_vec_broken` keeps `name : type` together

The comment guard pushes a commented `defstruct` field vector onto the broken
path, and that path split every `name` from its `: type`, one per line --
violating the house rule against splitting a name/type pair, and doing so
*already*, for any long defstruct, independent of comments:

```turmeric
(defstruct CookieOpts
  [name
   : cstr
   value
   : cstr
   ...
```

`fmt_vec_broken` now keeps an `F_TYPE_ANN` element on the line of the element
it annotates, the same rule `fmt_vec_params_broken` applies to parameter
vectors. Deliberately narrower than `param_is_annotation`, which also treats
`F_LIST` and `F_KEYWORD` as annotations: in a *generic* vector those are
ordinary elements (`[(f x) (g y)]`, `[:a :b]`), and gluing them would produce
an overlong line in exactly the case that broke.

Blast radius was one file: `stdlib/httpd.tur` (`CookieOpts`, `CorsOpts`),
reformatted here so FT7 (`fmt-bootstrap-stdlib`) stays green.

### Coverage (fix direction 4)

Three cases added to `tests/run-fmt.sh`:

- `fmt-preserves-comments-in-vectors` -- the report's minimal repro, asserting
  each of the four interior comments survives **and** that pass 2 == pass 1.
  Both halves matter: the non-idempotence is downstream of the loss, so a fix
  that stopped only the collapse would pass an idempotence-only check while
  still deleting the comments.
- `fmt-trailing-comment-stays-on-its-line` -- the relocation case above.
- `fmt-comment-before-close-bracket` -- the bracket-swallowing case above.

### Fix direction 5 (the guestbook files)

Verified rather than re-formatted. `store.tur` and `conts.tur` now round-trip
with byte-identical comment counts (48 and 63) and are idempotent; the only
remaining diff on either is column alignment inside the field vectors, not
content. They are left as-authored -- `examples/` is not under the FT7
bootstrap rule, and reformatting them would only churn alignment.
