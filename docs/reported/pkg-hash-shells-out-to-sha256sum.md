# The spice integrity hash was `tar -c | sha256sum`

**Severity: medium.** The lockfile's `:sha256` was a digest of a tar stream
produced by a subprocess. It was unstable on the machine that wrote it,
different on every other machine, and unavailable on Windows -- where the
failure degraded to storing a git SHA that could never verify against anything.

Found 2026-09-06 while making the package manager work on Windows. Fixed in the
same change.

## What it was

```c
buf_printf(&cmd, "tar -c '%s' 2>/dev/null | sha256sum", dir);   /* shasum -a 256 on macOS */
FILE *f = popen(cmd.data, "r");
```

`pkg_fetch_all` wrote that value into `tur.lock`; `tur run` recomputed it and
reported `integrity check failed for '<name>'` on a mismatch.

Four problems, and only the last was visible:

1. **It hashed `.git`.** The fetched tree is a shallow clone, and
   `.git/index` records file mtimes -- so running *any* git command inside a
   fetched spice changed its hash and the next `tur run` reported tampering on
   a tree nobody had touched.
2. **It hashed the absolute path.** `tar -c /abs/path` puts the path in the
   member headers, so the digest depended on where the project lived on disk.
   Two developers with the same content at different paths never agreed.
3. **tar output is not reproducible across implementations.** bsdtar and GNU
   tar disagree on default format, and both record uid/gid/mtime, so a lockfile
   written on macOS could not verify on Linux.
4. **No Windows host can run it.** MinGW ships neither `shasum` nor
   `sha256sum`, and popen there runs cmd.exe, where the `'%s'` is not quoting
   at all. `pkg_sha256_dir` returned false, `pkg_fetch_all` stored the git SHA
   as a fallback, and `tur run`'s check quietly never ran.

## The fix

A content hash, computed in process. For each regular file, in ascending byte
order of its relative path:

```
"F\0" <relpath> "\0" <decimal size> "\0" <bytes>
```

Separators normalized to `/`; the length prefix keeps a path and the bytes that
follow it from sliding into each other; the sort makes readdir order
irrelevant. `.git` is skipped as metadata, and `build/`, `.tur-abi-cache/` and
`.tur-repl-cache/` as build outputs that only appear after someone builds in the
tree.

File modes are deliberately absent: Windows cannot represent the executable bit,
and a hash that disagreed across platforms would be worse than one that ignores
it.

SHA-256 itself moved to `src/runtime/sha256.c` as a streaming implementation.
There were two copies of the algorithm in the tree (`image.c` had its own, which
slurped the whole file into a padded buffer) and one subprocess; `image.c` now
delegates, so there is one.

### Versioning, so an old lockfile is not read as tampering

The stored value now carries its algorithm: `tree1:<64 hex>`. `tur run` compares
only values that carry the tag (`pkg_hash_comparable`). Everything an older tur
could have written -- a bare tar digest, or the 40-hex git-SHA fallback -- is
skipped rather than reported as an integrity failure, and the next `tur fetch`
rewrites it in the current format.

The git-SHA fallback is kept for provenance and is untagged for the same
reason: it is not this algorithm's output and must never be compared as if it
were.

## What is guaranteed, and what is not

Reproducible for a given checkout: same content, same hash, regardless of path,
readdir order, or what git has been doing in the tree. That is what the check
needs, and it is what the old one did not have.

**Not** guaranteed byte-identical across platforms for a tree containing
symlinks or CRLF-translated files -- git materializes those differently, so no
content hash of a working tree can be. That limit is pre-existing and strictly
narrower than before; the tag is what keeps it from turning into a false
tampering report.

## Tests

- `tests/sha256_unit.c` -- FIPS 180-4 vectors (so the digest stays checkable by
  any other tool), streaming-equals-one-shot at all 201 splits of a 200-byte
  input, the 55/56/64-byte pad boundaries, and `tur_sha256_file` over more than
  one read buffer.
- `tests/unit/pkg_hash.c` -- the tree-hash properties above, one test each:
  path independence, `.git` and `build/` insensitivity, content and rename
  sensitivity, the path/content ambiguity case, and the tag rules.
