# A text-mode read rejects a file tur itself wrote

**Severity: high, Windows only.** `tur.lock` was never read on Windows. Not
"read partially" -- `pkg_lock_read` returned false on every call, and its caller
reads that as "there is no lock file yet", so the integrity check never ran, the
pinned `:resolved` refs never applied, and nothing said a word.

Found 2026-09-06 while building an end-to-end test for the spice integrity
hash: the check refused to fire on a tree that had plainly been tampered with,
and the hash was correct -- the lock entry was simply not there.

## The pattern

```c
FILE *f = fopen(path, "r");        /* text mode */
fseek(f, 0, SEEK_END);
long sz = ftell(f);                /* bytes ON DISK, CRLF included */
rewind(f);
if (fread(src, 1, sz, f) != (size_t)sz) { ...; return false; }   /* short: CR stripped */
```

On Windows a text-mode handle translates CRLF to LF on the way in, so `fread`
returns one fewer byte per line than `ftell` reported and the strict `!= sz`
check rejects the file. Every `tur.lock` tur writes has CRLF line endings --
`fprintf` on a text-mode handle put them there -- so tur was rejecting its own
output.

The failure is silent by construction: each of these call sites treats a false
return as "absent", which is the correct reading of a genuinely missing file and
the wrong reading of this.

## Where

Three sites had the strict form. All now open `"rb"`; the Turmeric reader treats
CR as whitespace (`reader.c:143`), so the extra bytes parse fine.

| site | what was lost |
| --- | --- |
| `pkg.c` `pkg_lock_read` | the whole lockfile: integrity check, pinned refs, transitive dep records |
| `pkg.c` `pkg_cmake_manifest_read` | cmake dependency resolution records |
| `cli/lsp_lite.c` `load_stdlib_docstrings` | the LSP-lite docstring table, so hover returned nothing |

`justrun.c`'s `jr_read_file` has the same `fopen(path, "r")` + `ftell` shape but
is **deliberately left alone**: it is tolerant (`buf[n] = '\0'` at the short
length, no size comparison), so it works, and text mode is what a Justfile
parser wants anyway. Changing it would hand that parser CR bytes it has no
reason to expect.

## What to watch for

The bug is in the *combination*, not either half:

- `fopen(..., "r")` + `ftell` size + a strict `fread` comparison is broken on
  Windows for any file with CRLF.
- `fopen(..., "r")` + a tolerant read is fine.
- `fopen(..., "rb")` + either is fine.

So the grep that finds it is not "which files open in text mode" but "which
text-mode reads compare the byte count against `ftell`".
