# `tur fmt` collapses multi-line inline-C onto the ```` ```c ```` fence line

## Summary

Severity: ergonomics gap (not a miscompile). The formatter emits every
inline-C block as `` ```c <code> `` -- the first line of C is glued onto the
opening fence, even when the body spans many lines. For multi-line C bodies
this produces source like:

```turmeric
  ```c struct { bool is_ok; int64_t ok_val; int64_t err_val; } *r = ...;
  if (!r) return 0;
  ...
  ```)
```

The code is still read back correctly by the Turmeric reader (it skips
whitespace after the `c`), and the layout is idempotent, so this is not a
correctness bug. But when such a `.tur` snippet is embedded in a Markdown doc,
a Markdown renderer treats `c struct { ... } *r = ...;` as the fence's *info
string*, so the first code line vanishes from the rendered block. It also reads
worse than the hand-authored convention of putting `` ```c `` on its own line
for multi-line bodies.

## Repro

```sh
printf '%s\n' \
  '(defn f [x : int] : int' \
  '  ```c' \
  '  int64_t y = x + 1;' \
  '  return y;' \
  '  ```)' > /tmp/c.tur
./build/tur fmt --stdout /tmp/c.tur
```

Observed:

```turmeric
(defn f [x : int] : int
  ```c int64_t y = x + 1;
  return y;
  ```)
```

Expected (hand-authored convention, see e.g. the historical `stdlib/result.tur`
before the bootstrap regen):

```turmeric
(defn f [x : int] : int
  ```c
  int64_t y = x + 1;
  return y;
  ```)
```

## Root cause

`src/compiler/fmt.c`, the `F_CBLOCK` case in `fmt_form` (and the parallel
`fmt_form_flat`):

```c
case F_CBLOCK:
    fs_puts(s, "```c ");
    fs_write(s, f->as.cblock.p, f->as.cblock.len);
    fs_puts(s, "```");
    break;
```

The captured block (`reader.c:read_cblock`) skips the whitespace after `c`, so
`cblock.p` starts at the first code character with no leading newline. The
formatter unconditionally prefixes `` ```c `` (with a trailing space), so the
first code line always lands on the fence line.

## Proposed fix

When the trimmed block contains a newline (i.e. it is genuinely multi-line),
emit `` ```c `` on its own line, then a newline + body-column indent, then the
block verbatim (its trailing `\n<indent>` already positions the closing fence):

```c
/* multi-line */
uint32_t cb_indent = s->col;
fs_puts(s, "```c");
fs_newline_indent(s, cb_indent);
fs_write(s, cp, clen);
fs_puts(s, "```");
```

Single-line bodies keep the current `` ```c <code> `` form.

### Caveat (why it was not fixed in the bootstrap-regen change)

The block's continuation lines carry their *source* indentation verbatim
(the reader does not re-indent them). The fix above round-trips cleanly only
when those continuation lines are already indented to the form's body column.
The current stdlib was authored inconsistently -- some files (`list.tur`) put
multi-line C on the fence line, others (`result.tur`) put `` ```c `` on its own
line -- so adopting the multi-line layout requires a coordinated reformat and a
decision on the canonical continuation indent. The bootstrap regen therefore
kept the existing single-line-fence behavior to avoid widening the change.

## Validation

Format a file with a multi-line inline-C body, confirm the opening `` ```c ``
is alone on its line, the body is unchanged, the closing `` ``` `` shares the
line with `)`, and `tur fmt` is idempotent. Run `bash tests/run-fmt.sh` and
the compiled-fixture suite.
