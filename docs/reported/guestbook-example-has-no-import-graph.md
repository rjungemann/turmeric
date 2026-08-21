# The guestbook example has rotted through: no import graph, and stale syntax in every file

**Severity: medium** (a shipped, CMake-registered example that cannot be built
at all). Split out of `docs/archive/examples-tree-does-not-run.md` on
2026-08-21, which is where the "seven guestbook files fail `tur check`" row
came from. Those seven are one problem, not seven, and the problem is bigger
than the invocation.

## Repro

```sh
for f in examples/guestbook/src/*.tur; do
  printf '%-38s ' "$f"; ./build/tur check "$f" >/dev/null 2>&1 && echo OK || echo FAIL
done
```

All seven FAIL. So does the way its own build invokes the compiler:

```sh
./build/tur emit-c examples/guestbook/src/main.tur
# examples/guestbook/src/main.tur:29:24: error: extern-c: parameter must be a symbol
```

That second command is the one `examples/guestbook/CMakeLists.txt` runs, so
the `guestbook` target has not built against a current `tur` for a long time.

## Three layers, in the order they have to be fixed

### 1. There is no import graph

`main.tur` contains no `import` and no `load`, yet the CMakeLists compiles
**only** `main.tur` and lists the other six merely as `DEPENDS`:

```cmake
COMMAND "${TUR_BINARY}" emit-c "${CMAKE_CURRENT_SOURCE_DIR}/src/main.tur" ...
DEPENDS tur ${TUR_SOURCES}
```

`DEPENDS` controls *when* the command re-runs, not what the compiler reads.
Nothing ever tells `tur` that `router.tur`, `store.tur`, `handlers.tur`,
`conts.tur`, `templates.tur` and `security.tur` exist. So this was never a
question of checking one member of a project in isolation -- there is no
project to check it against.

Two ways out, and the choice should be deliberate:

- give it a `build.tur` and build the directory (`tur build examples/guestbook`),
  which is the shape the rest of the toolchain expects; or
- add explicit `(load ...)` forms to `main.tur` in dependency order, which is
  what `examples/snake` does and what the CMake command already assumes.

### 2. Stale syntax, at least three distinct kinds

Fixing the graph will not make it compile. Each of these is independent:

| File | Error |
|---|---|
| `main.tur:29` | `(extern-c httpd-start [(port : int64)] : int64 ...)` -- "extern-c: parameter must be a symbol". The parenthesised `(name : type)` parameter form is not what `extern-c` takes. |
| `router.tur:28` | `: unit` -- there is no `unit` type. `:nil` is the spelling. |
| `store.tur:26` | `(definstance Serializable GuestEntry ...)` -- the typeclass is in `stdlib/serial.tur`, which nothing loads. |

Expect more once those are past; the files have clearly not been compiled in a
long time, and the sweep only ever reported each file's FIRST error.

### 3. It depends on serializable continuations

`conts.tur` and `router.tur` are built on `serial-resume` /
`serial-cont->bytes` / `bytes->serial-cont` -- which is the subject of the open
report
[serializable-continuations-aspirational-surface](serializable-continuations-aspirational-surface.md):
that surface is documented in four guides and **not implemented**. So the
guestbook cannot run even with every syntax error fixed, and this is the layer
that decides whether the example is worth repairing now at all.

## Fix direction

Answer layer 3 first -- it is the one that can make the other two wasted work.
If serializable continuations are not landing soon, the honest move is to say
so in the example's own README and in the four guides that quote it, rather
than leaving a CMake target that has not built in months. If they are landing,
fix in the order 1 -> 2 -> 3 and add the example to the run-ratchet in
`tests/check-examples.sh` (it needs a listener, so it will want a row in
`examples/examples-run-baseline.txt` with that reason, or a smoke mode that
starts and immediately stops the server).

Either way the seven baseline rows in
`examples/examples-check-baseline.txt` should collapse to one row for the
project, not seven for its members, once the invocation question is settled.

## Guides to update when fixed

- docs/guides/serializable-continuations-guide.md (and the three other guides
  that quote this example) -- see the report linked in layer 3.
