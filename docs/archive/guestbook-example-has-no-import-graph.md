# The guestbook example has rotted through: no import graph, and stale syntax in every file

**Severity: medium** (RESOLVED 2026-09-02; a shipped, CMake-registered example that cannot be built
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

## Resolution (2026-09-02)

Layer 3 landed first (`serializable-continuations-aspirational-surface`:
`serial-cont->bytes` / `bytes->serial-cont` / `serial-resume` are real), so
the example was worth repairing, and it was rewritten rather than patched:

1. **Import graph.** `examples/guestbook/build.tur` makes it a spice
   (`:c-sources ["httpd.c"]` links the shim), every file is a `defmodule`
   with explicit `import`s, and `main.tur` is the root. `tur check <file>`
   works on each member, `tur run src/main.tur` and `tur build
   examples/guestbook` both produce the server, and the CMake target emits
   one C unit from `main.tur` (auto-spice discovery resolves the imports).
   `examples/CMakeLists.txt` now actually adds the subdirectory; the old
   target linked a `turi_static` that does not exist.
2. **Syntax.** Rewritten in the shipped language: `extern-c`-free inline-C
   bindings (`httpd.tur`), `int`/`cstr`/`ptr<void>` types, `(Option cstr)` /
   `(Result serial-cont cstr)` via the typed inline-C builders, `match` arms
   without arrows, `cond` with `:else`, `while` loops. HMAC-SHA256, percent
   decoding, form parsing and the entries file are small inline-C helpers in
   `strutil.tur` / `security.tur` / `store.tur`. `httpd.c` itself declared
   no POSIX feature macros, so `strdup` came back truncated to `int` and the
   first request segfaulted -- fixed.
3. **Continuations, as the capture grammar allows.** One continuation per
   page: `(serial-reset (message-submitted name (serial-shift
   suspend-message-page 0)))` -- the frame carries the page's state as its
   cstr env, the POST body arrives through the int hole (as its address), the
   receiver stores the bytes under a signed token and sends the form, and
   `POST /submit?k=TOKEN` rebuilds and resumes. Two rules found on the way,
   now in the guide's "Capture scope": the context callee (and everything it
   calls) must be uncolored, so the `*-submitted` leaves return a step code
   and `advance` starts the next page's reset outside any reset; and the
   receiver must be a named uncolored function (a capture-free lambda is
   rejected -- `serial-shift-non-capturing-lambda-receiver-rejected`). Back
   navigation is one continuation with a `decision` field, not two tokens.

Two compiler defects surfaced and are fixed in the same change: a
`serial-reset` inside a `defmodule` body was invisible to the preamble
predicate, so module programs were emitted without the serial runtime
(`tests/fixtures/serial-reset-in-defmodule`); and the context collector's
sixty rejections were anonymous (`BODY-UNSUPPORTED ?`) -- `TUR_TRACE_CORE=1`
now prints `[CTX-REJECT] cps_ir.c:<line>`.

Coverage: `tests/run-guestbook.sh` (ctest `tur_guestbook_smoke`) builds the
example, serves nine requests on a private port and walks the whole flow
with curl -- name, message, Back, new message, Confirm, a tampered token
refused, `/entries` -- 10/10; the seven `examples-check-baseline.txt` rows
are gone (38/0 with the manifest excluded from the sweep) and `main.tur` has
its run-baseline row. The web-continuations guide and tutorial describe the
code that ships.
