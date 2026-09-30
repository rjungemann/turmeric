---
title: Security Guide
category: Security
description: What Turmeric promises to handle safely -- the trust boundary for each input, the promise made at it, and where the implementation does not yet keep that promise
---

# Security Guide

Turmeric is a compiler, an interpreter, a runtime library, a package fetcher, a
web playground, an installer, and a release pipeline. Each has a different
notion of "untrusted input". This guide says which inputs the project promises
to handle safely, so that a bug report has something to be graded against.

Read this before filing a security report -- see
[`SECURITY.md`](https://github.com/rjungemann/turmeric/blob/main/SECURITY.md)
for how. The difference between a bug and a non-bug here is usually the
boundary, not the crash.

Each promise below carries its **status today**. Where a promise is not yet
kept, the gap is named. A promise with no status note is one the implementation
keeps.

---

## The five boundaries

| # | Boundary | Untrusted input | Promise |
| --- | --- | --- | --- |
| T1 | Compiling a project | a `.tur` tree, its `build.tur`, its `spices/`, its Justfile | Split -- see below. `tur build` makes **no promise**. `tur check`, `tur run --list` and the language server promise not to execute repo-supplied code or shell unless you asked them to. |
| T2 | A compiled program's own inputs | bytes handed to stdlib readers: `bytes->serial-cont`, image files, JSON, HTTP requests to `httpd`, `read-async` lengths | A malformed input is a `result` error or a panic -- never a wild read or write. Well-formed is not authentic: authenticating bytes is the program's job. |
| T3 | The sandboxed interpreter | the program text evaluated inside `Env/new-sandboxed`, the macro environment, or the playground | A capability-denied environment does no I/O, no filesystem, no process, no environment, no FFI, no inline C; it cannot corrupt or end the host process; and it terminates under fuel. |
| T4 | The supply chain | the installer, release assets, `tur fetch` of a `:url` spice, Actions inputs | Installing a release gets you the bytes CI built, verifiably. A spice pinned in `tur.lock` cannot change under a rebuild without a diagnostic. |
| T5 | Editor protocols | LSP, DAP and MCP messages over stdio | The peer is your editor, so it is semi-trusted -- but framing must be robust. A bad `Content-Length` must not overflow. |

---

## T1 -- Compiling a project

This boundary splits, and the split is the most important thing in this guide.

### `tur build` makes no promise

**Building a Turmeric project runs that project's code.** A tree you build can
execute arbitrary code through at least:

- an inline C block in any module, including a fetched spice;
- a compile-time macro (see T3);
- its Justfile, when you invoke a recipe;
- `:link-flags` in `build.tur`, which is documented as the verbatim sibling of
  `:link-libs` -- no prefix is added, because that is the only way to spell
  `-framework Cocoa` -- and which therefore lands in the compiler command line
  as written;
- `:c-sources`, which names C files to compile into your binary;
- `:cmake-deps`, which runs upstream CMake.

What a manifest and an inline-C `__tur_autolink__` marker may contribute to
that command line is now a fixed vocabulary -- `-l<name>`, `-L<dir>`,
`-I<dir>`, `-D<key>[=<val>]`, `-framework <name>`, `-Wl,<...>`, a source or
object path, or one of a short list of bare toolchain flags -- and anything
else is a build error naming the token. That closes the *shell*: a manifest
cannot smuggle `; touch x` into the command any more. It does not change the
promise, because the vocabulary is itself enough to run code: `-l` names a
library whose static initializers run, a `.c` path is compiled into your
binary, and `-Wl,` speaks directly to the linker. It is a narrower channel,
not a closed one, which is why `tur build` still promises nothing.

This is not a defect list. It is the same position every compiler takes.
`build.tur` is Turmeric's `.cargo/config.toml`: Cargo honours a repo's
`rustflags` and `[target.*] runner` and documents that building a crate runs
`build.rs` and its proc macros; `make` runs a Makefile. Turmeric is in the same
place, and naming the analogy is more useful than a severity number would be.

**So: do not `tur build` a tree you would not `make`.** Read a new dependency's
`build.tur` the way you would read its `build.rs`.

### `tur check`, `tur run --list`, and the language server do promise something

These are different, because an editor runs them on a tree you have merely
*opened*. The promise is the one clangd makes, and it is narrower than "safe":

> **They do not execute repo-supplied code or shell unless you asked them to.**

Two things stand between that promise and the implementation today.

#### `tur check` expands macros, and the macro environment is not yet a boundary (open, high)

`tur check` expands compile-time macros, which is the same exposure Rust has
with proc macros. The macro environment is capability-denied, and that is now
enforced for every native function as well as the builtins (T3): a
`defmacro*` body that calls `process/spawn`, deletes a file or reads the
environment gets a diagnostic, and nothing runs.

It is still not a boundary against a hostile tree, for the reason T3 gives: a
`defmacro*` body can forge a handle and read or write an arbitrary address in
the compiler's process (S-5). When that is fixed, a genuinely
capability-denied macro environment will be a *better* story than Rust's, and
this guide will promise it.

Until then there is an opt-out. The global flag `--no-proc-macros` refuses
every `defmacro*` with a diagnostic, so no macro-time code runs -- what
rust-analyzer ships as `procMacro.enable = false`:

```
tur --no-proc-macros check src/
```

Template `defmacro` still expands, because substitution runs nothing. Without
the flag, **`tur check` on an untrusted tree is as exposed as `cargo check` on
an untrusted crate.**

#### `tur repl` auto-discovery compiles and dlopens (open, medium)

`tur repl` walks up from the working directory for a `build.tur`, builds the
tree into a shared library under `.tur-repl-cache/`, and `dlopen`s it. That is
Gradle-tier behaviour and this guide makes **no promise** about it.

What it does guarantee is that the object loaded is one *this* `tur` built: the
cache carries a `.built-by` sidecar recording the compiler's version, path,
size and mtime, and a mismatch forces a rebuild. So a repository that commits a
`.tur-repl-cache/lib-N.so` does not get it loaded.

The opt-out, `TUR_NO_AUTO_SPICE=1`, is still default-allow, which points the
wrong way: pnpm, Bun, Deno and Neovim have all moved to default-deny plus an
allowlist. The intended replacement is direnv's model -- hash the tree's
`build.tur`, ask once, remember the answer -- which would turn auto-discovery
from something this guide declines to promise into a documented design.
Tracked as D-3 in the
[security audit plan](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/security-audit-plan.md).

### Editor trust support

Whatever the CLI promises, an editor integration should declare its posture so
the editor can enforce it. Both VS Code extensions in this repository declare
`capabilities.untrustedWorkspaces`:

- `vscode-syntax-ext` (highlighting, formatting, LSP client) is **limited**,
  with `turmeric.serverPath` listed as a restricted configuration. In VS Code's
  Restricted Mode the workspace cannot redirect the language server to another
  executable; highlighting still works.
- `editors/vscode-turmeric` (debugging over DAP) is **limited**. Its adapter
  executable comes from the workspace's launch configuration, so debugging a
  repository means trusting it.

That gives Restricted Mode users the same protection Go's extension gives them,
and it costs two blocks of JSON.

---

## T2 -- A compiled program's own inputs

A program you wrote, reading bytes it did not choose: a saved continuation, an
image, a JSON body, an HTTP request. **The stdlib reader must not corrupt memory
on any input.** A malformed input is a `result` error or a panic, never a wild
read or write. This is the ordinary promise a runtime library makes.

What the stdlib readers do:

- **Serialized continuations.** Every route that rebuilds one --
  `bytes->serial-cont`, `resume-cont!`, `image/blob-resume!` -- runs the same
  check inside the runtime: each record must fit the buffer, carry a known tag,
  and, for a call frame, name a frame this program registered, with the
  environment kind that frame was registered with. `bytes->serial-cont` turns a
  bad buffer into an `Err`; the others panic.
- **Images.** The header's payload length is held to the file's real size, a
  CRC covers the payload as well as the header, and the continuation is checked
  before the image counts as loadable -- a damaged image is a cold start.
- **JSON.** Both decoders (compiled and interpreter) cap nesting at 256, decode
  `\uXXXX` (a lone surrogate or `\u0000` is an error), and free what they built
  when they fail.
- **`httpd`.** A malformed, conflicting or oversized `Content-Length`, and any
  `Transfer-Encoding`, is refused before a byte of the body is read; the body
  cap defaults to 8 MiB (`httpd-set-max-body!`). Servers bind loopback unless
  the program asks for more. See
  [httpd-guide](httpd-guide.md#binding-and-request-limits).

These readers, along with the LSP framing and the compiler's own front door
(the reader, the manifest reader, the Justfile parser), run nightly under
libFuzzer with ASan and UBSan -- see `tests/fuzz/README.md`.

**Checked is not authenticated.** A continuation buffer that passes the check
still rebuilds a continuation of *this program's* frames with whatever
environment values the buffer carries, and an image's CRCs catch corruption,
not tampering -- anyone who can write the file can recompute them. Do not
resume bytes that crossed a trust boundary without authenticating them first;
the guestbook example keeps continuations server-side and hands the client only
an HMAC-signed name. A `Serializable` instance's own `deserialize`, which
receives an environment's bytes, is the program's code and the program's
responsibility.

---

## T3 -- The sandboxed interpreter

`turi_env_new_sandboxed()` and the compile-time macro environment both promise:
no I/O, no filesystem, no process spawning, no environment variables, no FFI,
no inline C, no unsafe memory, no way to corrupt or end the host process, and
termination under a step-fuel bound. See the
[Sandboxing Guide](sandboxing-guide.md) for the embedding API.

**Capabilities are enforced.** Every native function the interpreter ships
has a row in one classification table that names the capability it requires,
and the single native dispatch refuses a call whose environment lacks it -- by
name from Turmeric, via `turi_call` from C, and through a higher-order native
alike. A new native without a row fails the sandbox test. The
[Sandboxing Guide](sandboxing-guide.md#capability-classification) has the
classes and the rows that are not pure. `load` and `import` are refused
outright, and the `extern-c` overrides for `printf`, `getenv` and `exit` need
FFI like every other `extern-c`.

**Status today: memory safety is not kept (S-5, open, high).** Most natives
take a collection, string or continuation handle as a bare integer and cast it
to a pointer, and nothing checks that the integer came from the matching
constructor. So sandboxed text can forge one:

```
(vec-get 4096 0)   ; reads address 4096
```

That is a wild read, and the setters make it a wild write, so an adversary
who can guess an address has the host process. It needs no capability. It is a
property of the interpreter's value model rather than of any one native.

A panic, by contrast, no longer ends the host. In an environment without
`TURI_CAP_PROC`, a panic that nothing catches, and the error exits of natives
like an out-of-bounds `vec-get`, come back to the embedder as a `TURI_ERROR`
reading `panic: <msg>`, and the environment stays usable. A panicking
`defmacro*` is an ordinary expansion diagnostic.

Until S-5 is fixed, **do not treat `Env/new-sandboxed` as a boundary against
hostile code.** It is now a sound boundary against *careless* code -- a plug-in
cannot open a file, spawn a process, or read the environment, however it
spells the call -- but not against code written to corrupt memory.

### Try Turmeric

The web playground runs with all capabilities granted, deliberately. Its
boundary is the browser's, and the WebAssembly build has no filesystem and no
process spawning to reach. That is an acceptable posture for a playground and
this guide records it as intentional rather than a gap.

What the site does promise is the ordinary web one: **text you did not write --
a pasted file, an opened project zip, a restored tab, a docs page -- is shown
as text and never runs as script in the page.** Three things keep it:

- **A Content-Security-Policy on every response from turmeric-lang.com**,
  defined once in `web/csp.js`. Its `script-src` has no `'unsafe-inline'`, so
  markup that gets past escaping does not execute. It allows WebAssembly
  compilation (`'wasm-unsafe-eval'`), mermaid from one jsDelivr path, the doc
  pages' web fonts, and inline *styles* -- Monaco needs them -- and it refuses
  framing (`frame-ancestors 'none'`). The generated doc pages load their
  scripts from files for the same reason.
- **Escaping that holds in attributes as well as in element content**, so a
  value interpolated into `value="..."` cannot add attributes of its own.
- **A console transcript stored as data.** What the playground keeps in
  `localStorage` is rebuilt with DOM calls on load, so nothing read back from
  storage is ever parsed as markup.

A program that never returns does not take the playground with it. After a
second a **Stop** button ends it; after 30 seconds the playground stops it
itself. Either way the interpreter restarts in a fresh session, and the
definitions from earlier runs are gone.

`'wasm-unsafe-eval'` is understood from Chrome 97, Firefox 102 and Safari 16.
An older browser that also applies CSP to WebAssembly compilation will not load
the interpreter.

**The documentation is trusted content.** The in-app docs pane and the pages
under `/docs/html/` are HTML generated from this repository's guides and
docstrings and from the READMEs and docstrings in `turmeric-spices`
(`tools/genguides.py`, `gendocs.py`, `genspices.py`). Markdown passes raw HTML
through, so a spice whose README carries HTML puts that HTML on
turmeric-lang.com, in the playground's origin. The policy stops it running
script; it does not stop it restyling or rewording the page. The defence is
review: read a documentation change to `turmeric-spices` as a change to the
site.

---

## T4 -- The supply chain

**Installing a release should get you the bytes CI built, verifiably. A spice
pinned in `tur.lock` should not change under a rebuild without a diagnostic.**

**Status today: neither half is kept.**

### The installer builds `main` (C-1, open, high)

The advertised install path --
`curl -sSf https://turmeric-lang.com/install | sh` -- runs
`brew install --HEAD`, and the Homebrew formula is `head`-only: no `url`, no
`sha256`. So it compiles whatever `main` is at that moment. A bad afternoon on
`main` reaches every new install, and there is no checksum anywhere in the path.

Release assets *do* exist, with a `sha256sums.txt`, but nothing installs from
them except `tvm`. If you want a verified install today, use `tvm` or download
a release tarball and check it by hand, as the
[installation guide](releases-and-installation-guide.md) describes.

### `tvm`'s checksum check can be skipped silently (C-2, open, medium)

`tvm install` verifies a downloaded asset against the release's
`sha256sums.txt`, but the check has four paths that skip it, and only one of
them says so:

| Condition | What happens |
| --- | --- |
| `sha256sums.txt` missing, unreachable or empty | skipped, **silently** |
| the asset has no row in that file | skipped, **silently** |
| no `sha256` tool on the system | skipped, with a log line |
| an explicit `--from` source | skipped |

It should fail closed. Note also that the sums file is fetched from the same
origin as the asset, which makes this an integrity check, not an authenticity
one -- and that release assets are currently unsigned, with no build
provenance attestation (C-4).

### `tur.lock` is trust-on-first-use (C-3, open, medium)

The hash in `tur.lock` is recomputed and **overwritten on every fetch**, so it
records what you last downloaded rather than what you agreed to. It can
therefore catch a local edit to `spices/` after a fetch; it cannot catch
upstream changing under you.

The single comparison in the tree runs in `tur run` only -- `tur build` does not
check -- and it is skipped when the dependency directory is absent (that path
fetches and rewrites the hash) and when the recorded hash predates the current
algorithm.

`tur audit` lists origins; it does not verify them, and says so.

So: **pin `:ref` to a tag rather than a branch, and read a new spice before you
add it.** A `:cmake-deps` entry is a trust decision equivalent to running build
scripts from that repository.

### Workflows

No workflow action is pinned to a commit SHA, `ci.yml` has no top-level
`permissions:` block, and several toolchain installs float (C-5, C-6). This
matters because the release pipeline is what T4's first promise depends on.

---

## T5 -- Editor protocols

The peer is your own editor, so it is semi-trusted -- but **message framing
must be robust**.

`tur lsp` and `tur dap` accept a `Content-Length` of plain decimal digits, at
most 64 MiB, after a header block of at most 8 KiB; anything else ends the
session as a framing error.

---

## Out of scope

- Denial of service by a developer's own program against their own machine.
- The R7RS embedding's continuation memory growth, which is tracked as an
  ordinary bug.
- The Godot bindings, which live in a separate repository.

---

## What the effect system does and does not promise

Nothing, today, for security. `--strict-effects` defaults off and only warns.
Inline C outside an `Unsafe` effect row is a lint behind
`--lint-inline-c-unsafe`, also default off. The deserializers discussed under
T2 infer plain effect rows.

So `#fx{Unsafe}` is documentation and a lint, not a boundary. Whether it should
come to mean "may corrupt memory on bad input" is an open question in the audit
plan; until it is answered, do not read its absence as a safety claim.

---

## Reporting

Private advisory form:
<https://github.com/rjungemann/turmeric/security/advisories/new>. See
[`SECURITY.md`](https://github.com/rjungemann/turmeric/blob/main/SECURITY.md).

The open items above are the audit's own backlog, tracked in the
[security audit plan](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/security-audit-plan.md).
Reporting one of them again is welcome but will not be news.
