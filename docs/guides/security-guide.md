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
| T2 | A compiled program's own inputs | bytes handed to stdlib readers: `bytes->serial-cont`, image files, JSON, HTTP requests to `httpd`, `read-async` lengths | A malformed input is a `result` error or a panic -- never a wild read or write. |
| T3 | The sandboxed interpreter | the program text evaluated inside `Env/new-sandboxed`, the macro environment, or the playground | A capability-denied environment does no I/O, no process, no FFI, no inline C, and terminates under fuel. |
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

Three things stand between that promise and the implementation today.

#### `tur run --list` evaluates Justfile backticks (open, high)

A Justfile variable assignment may have a backtick command substitution:

```
commit := `git rev-parse HEAD`
```

`tur run` evaluates those while *parsing* the Justfile, and parsing happens
before listing -- so `tur run --list`, which reads in every toolchain on earth
as an inventory command, runs the shell. `--list` failing that surprise test is
a defect regardless of what this guide promises.

The fix is scoped: defer a backtick from parse time to the first recipe
invocation that actually uses the variable. Tracked as D-2 in the
[security audit plan](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/security-audit-plan.md).

Until it lands: **`tur run --list` is not safe on a tree you do not trust.**

#### `tur check` expands macros, and the macro environment is not yet a boundary (open, high)

`tur check` expands compile-time macros, which is the same exposure Rust has
with proc macros. The macro environment is *intended* to be capability-denied
-- it calls `turi_env_deny(env, TURI_CAP_ALL)` and bounds work with step fuel --
but see T3: the native function table is registered into every environment and
almost none of its entries consult the capability set, so the denial is not yet
enforced at the point that matters.

When that is fixed (S-1, below), a genuinely capability-denied macro
environment will be a *better* story than Rust's, and this guide will promise
it. It does not promise it yet.

There is also no opt-out today. `--macro-caps=io` exists and grants *more*, not
less; the `--no-macros` equivalent -- what rust-analyzer ships as
`procMacro.enable` -- has not been built.

Until then: **`tur check` on an untrusted tree is as exposed as `cargo check`
on an untrusted crate.**

#### `tur repl` auto-discovery compiles and dlopens (open, medium)

`tur repl` walks up from the working directory for a `build.tur`, builds the
tree into a shared library under `.tur-repl-cache/`, and `dlopen`s it. That is
Gradle-tier behaviour and this guide makes **no promise** about it.

Worse, whether to rebuild is decided by comparing mtimes, so a repository that
*commits* a `.tur-repl-cache/lib-N.so` newer than its sources gets that object
loaded with no build at all.

The opt-out, `TUR_NO_AUTO_SPICE=1`, is default-allow, which points the wrong
way: pnpm, Bun, Deno and Neovim have all moved to default-deny plus an
allowlist. The intended replacement is direnv's model -- hash the tree's
`build.tur`, ask once, remember the answer -- which turns this from a defect
into a documented design. Tracked as D-3.

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

**Status today: not kept for the serial/continuation and image paths (M-1,
open, high).** `tur_serial_cont_deserialize` bounds-checks nothing: frame count,
name length, string length and environment length are all trusted from the
input, and raw integers from the byte stream become frame environments.
`bytes->serial-cont` validates first, but shallowly, and two entry points --
`resume-cont!` and `image/blob-resume!` -- skip validation altogether. An
image's CRC covers its 68-byte header only, while the payload length read from
that header sizes a `malloc`.

This matters most where the project already ships T2 across a network: the
guestbook example resumes a continuation from a `POST` token. **Today a forged
token is a forged continuation.** Do not accept a serialized continuation from
an untrusted source.

Whether `bytes->serial-cont` should verify an HMAC -- integrity is not
authenticity -- is an open question in the audit plan.

Also open under T2: JSON has no nesting depth limit and over-reads on input
ending in a backslash (M-2); `httpd` does not cap a request body and does not
reject a request carrying both `Content-Length` and `Transfer-Encoding` (M-4);
several size computations are done in signed `int` (M-5).

---

## T3 -- The sandboxed interpreter

`turi_env_new_sandboxed()` and the compile-time macro environment both promise:
no I/O, no process spawning, no FFI, no inline C, no unsafe memory, and
termination under a step-fuel bound. See the
[Sandboxing Guide](sandboxing-guide.md) for the embedding API.

**Status today: the capability check is bypassed for native functions (S-1,
open, high).** A sandboxed environment is built by creating an ordinary
environment -- which registers the full native table, including process
spawning, file open and write, unlink, `getenv`, and raw-descriptor
`read-async`/`write-async` -- and then clearing the capability set. But the
natives do not consult that set. Exactly one of them does.

The capability checks that do exist cover the *builtin* dispatch (the
`println-*` family, `dlopen`/`dlsym`/`dlclose`, and the raw-memory
operations), the FFI thunk path, inline C, the `(async ...)` form, and
`import`. A native reached by name is not checked.

Two consequences worth stating plainly:

- `read-async` performs its read eagerly, before any future machinery, so the
  capability gate on the `(async ...)` *form* never intercedes.
- The macro environment denies every capability and still has the same table
  registered into it, which is why the T1 macro promise above is deferred.

The fix is one choke point, not two hundred checks: every native declares a
capability, and the native dispatch checks it. Until that lands, **do not treat
`Env/new-sandboxed` as a boundary against hostile code.** It is a boundary
against *accidents* -- a plug-in that calls `println` by mistake -- and it
stops the builtins listed above.

Two narrower gaps: `extern-c`'s known overrides for `printf` and `getenv` skip
the FFI check the thunk path enforces, and the interpreted `printf` takes its
format string from the program (S-2); the inline-C emulator's `snprintf` shim
coerces every argument to `long long`, so a `%s` in the body dereferences an
integer (S-3).

### Try Turmeric

The web playground runs with all capabilities granted, deliberately. Its
boundary is the browser's, and the WebAssembly build has no filesystem and no
process spawning to reach. That is an acceptable posture for a playground and
this guide records it as intentional rather than a gap.

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

**Status today: not kept (M-3, open, medium).** The LSP framing layer parses
`Content-Length` with an unchecked `atol`. A value of `-1` wraps the
`body_len + 1` allocation to zero, producing a `malloc(0)` followed by a huge
read -- a heap overflow. Header accumulation is also unbounded. The debug
adapter reuses the same reader.

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
