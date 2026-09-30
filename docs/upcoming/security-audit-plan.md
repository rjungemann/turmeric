# Security audit -- Turmeric as it stands at v0.56.3

> **Status: PROPOSED.** Written 2026-09-30 against `main` @ 81e12de4
> (v0.56.3). Nothing in this plan has been executed. Section 2 lists what a
> one-afternoon survey already turned up, so the audit starts from a map, not
> from zero; every row there is a *candidate* until the work package that
> owns it verifies or retires it. Items marked **verified** were reproduced
> or read in the source during the survey; the rest are read-only findings
> from the survey and still need a repro.
> **Type:** Security / process / tooling
> **Depends on:** nothing that is not already in the tree. The Debug build's
> ASan+UBSan (`CMakeLists.txt:33`), the four differential fuzzers
> (`tests/*-fuzz-src.py`, `.github/workflows/fuzz.yml`), and the `TSan`
> CMake config (`src/CMakeLists.txt:481`) are the tooling this plan builds on.
> **Related:** [sandboxing-guide](../guides/sandboxing-guide.md) (the
> promise WP3 audits), [consuming-spices-guide](../guides/consuming-spices-guide.md#security)
> (the promise WP7 audits), [ci-release-workflows-plan](hold/ci-release-workflows-plan.md)
> and [release-in-actions-plan](release-in-actions-plan.md) (WP7 lands
> alongside them), `docs/reported/README.md` (where findings go).

## 0. Summary

Turmeric is a compiler, an interpreter, a runtime library, a package
fetcher, a web playground, an installer, and a release pipeline. Each of
those has a different notion of "untrusted input", and today none of them
has a written one. The audit's first job is to fix that: **say which inputs
the project promises to handle safely**, then check the code against the
promise. Its second job is the ordinary one: find the memory-safety and
injection bugs in the C that handles those inputs, and put the cheap
hardening (pinning, permissions, checksums, CSP) in place.

The survey found no evidence of anything malicious and no exploited bug. It
did find that three documents promise more than the code delivers:

- `docs/guides/sandboxing-guide.md` presents `Env/new-sandboxed` as safe for
  untrusted code, but the natives table registered into *every* env
  (`src/turi/env.c:224-251`) includes `process/spawn`, file open/write,
  unlink and raw-fd read/write with no capability check (section 2, S-1).
- `docs/guides/consuming-spices-guide.md:405` says every fetched spice is
  verified and builds fail on mismatch; the lock hash is trust-on-first-use
  and rewritten on every fetch (`src/compiler/pkg.c:2572-2582`), and only
  `tur run` ever compares it (`src/main.c:5737-5750`) (section 2, C-3).
- The install path advertised in `README.md:20` is `curl | sh` into
  `brew install --HEAD`, which builds whatever `main` is at that moment
  (`web/worker.js:1-24`, `Formula/turmeric.rb:5`), not a release with a
  checksum (section 2, C-1).

The audit is eight work packages (section 3), ordered so that the first
two weeks close the items an outside reporter would find first, and so that
the fuzz harnesses and the CI hardening -- the parts that keep paying after
the audit ends -- land early rather than as an afterthought.

## 1. Trust boundaries -- what the audit is checking against

The audit needs a decision on each of these before it can grade a finding.
Proposed answers are given; section 7 lists the ones the author must
confirm.

| # | Boundary | Untrusted input | Proposed promise |
| --- | --- | --- | --- |
| T1 | **Compiling a project** (`tur build/run/check/emit-c`, `tur repl`, LSP, MCP, `tur run --list`) | a `.tur` tree, its `build.tur`, its `spices/`, its Justfile | Like every compiler: building a project *is* running its code (inline C, macros, Justfile). **No promise** that `tur build` on a hostile tree is safe. But `tur check`, the LSP, `tur run --list` and `tur repl` *auto-discovery* are things an editor runs on a tree you have only opened, and the macro env is documented as capability-denied -- those **should** be safe to point at an untrusted tree, and today are not (S-1, D-2, D-3). |
| T2 | **A compiled program's own inputs** | bytes handed to stdlib readers: `bytes->serial-cont`, image files, JSON, HTTP requests to `httpd`, `read-async` lengths | The stdlib reader must not corrupt memory on any input. A malformed input is a `result` error or a panic, never a wild read or write. This is the ordinary promise every runtime library makes; today the serial/image path does not keep it (M-1). |
| T3 | **The sandboxed interpreter** (`Env/new-sandboxed`, the compile-time macro env, Try Turmeric's wasm env) | the program text evaluated inside it | A capability-denied env can do no I/O, no process, no FFI, no inline C, and terminates under fuel. Today the natives table bypasses the capability check (S-1). Try Turmeric runs `CAP_ALL` and relies on the browser sandbox (`src/web/wasm_glue.c:159-162`) -- acceptable, but it must be written down. |
| T4 | **The supply chain** | the installer, release assets, `tur fetch` of a `:url` spice, GitHub Actions inputs | A user who installs a release gets the bytes CI built, verifiably. A spice pinned in `tur.lock` cannot change under a rebuild without a diagnostic. |
| T5 | **Editor protocols** (LSP, DAP, MCP over stdio) | framed messages from the editor | Peer is semi-trusted (it is the user's editor), but framing must be robust: a bad `Content-Length` must not overflow (M-3). |

Out of scope for this audit: DoS of a developer's own machine by their own
program; the R7RS embedding's memory growth (`docs/reported/r7rs-callcc-memory-never-freed.md`
covers it); Godot bindings (separate repo).

## 2. Findings already in hand

Severity is the survey's estimate against the section 1 promises, to be
re-graded by the owning work package. **verified** = reproduced or read
line-by-line during the survey; otherwise a read-only claim awaiting repro.

### Sandbox (WP3)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| S-1 | high | Natives that reach the OS are registered into every env, including sandboxed and macro envs, with **no capability check**: `process/spawn` (fork+execvp), `fs/tmpfile`, `io-fopen-read/write`, `write-temp-file`, `r7rs-io-open__`, `r7rs-unlink__`, r7rs `getenv`/environ, `json/decode-file!`, `read-async`/`write-async` on raw fds. **verified** in source (`src/turi/interpreter_natives.c:4851-4877, 5199, 4403-4412, 2615, 2892, 2858-2880, 1349`; `src/turi/fiber.c:754-830, 954-955`; registration `src/turi/env.c:224-251`; the sandbox constructor `env.c:277-286`; the macro env `src/turi/macro_env.c:212, 229`). The only cap checks are in `is_blocked_builtin` (`src/turi/eval.c:4070-4104`, println/dlopen/raw-memory only), the FFI thunk paths, inline-C, async and import. **Not yet reproduced from macro time**: four attempts via `defmacro` + `(import process :for-macros)` expanded correctly but never ran the native -- something in name resolution or the inline-C cap intercedes. The `Env/new-sandboxed` embedder API (`stdlib/turi/eval.tur:49`) and `tests/turi/sandbox-eval.c` are the right place for the PoC. |
| S-2 | medium | `extern-c` "known overrides" (`printf`, `printf_s`, `getenv`) skip the FFI cap check the thunk path enforces, and `printf`'s format string is program-controlled (`src/turi/eval.c:360-421`). | |
| S-3 | medium | The inline-C emulator's snprintf pattern hands the program's format string to `snprintf` with every argument coerced to `long long` -- a `%s` in the body dereferences an integer (`src/turi/eval.c:5532-5535`). | |
| S-4 | info | Try Turmeric's wasm env is `CAP_ALL` by design (`src/web/wasm_glue.c:159-162`); `tests/turi/sandbox-eval.c:37-88` covers only println/async/inline-C. | |

### Compiler driver and filesystem (WP2)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| D-1 | high (under T1 for `check`/LSP) | The C-compiler command line is assembled by `buf_printf` and run with `system()` **without quoting** any of: `TUR_CC_FLAGS`, output path, input paths, `-I` dirs, `__tur_autolink__` marker text scraped from generated C by `strstr` (`src/main.c:2283-2295`), `:link-libs`/`:link-flags` from the manifest (`main.c:2375-2392`), transitive spices' `:c-includes`/`:c-sources` (`main.c:4352-4390`), and cmake-dep flags (`src/compiler/pkg.c:4118-4136`). **verified** at `main.c:2873-2897`; same shape at `:2957-2977, :3079-3087, :6940-6982, :7595-7608`. `tur_shell_quote` (`src/platform_proc.h:109-159`) exists and is correct; it is just not used here. Any inline-C block in any module (a fetched spice included) can inject shell text through the autolink marker. |
| D-2 | high (under T1) | Justfile backtick assignments are evaluated while *parsing* (`src/compiler/justrun.c:1438-1440, 959, 1892, 1930`), and parsing precedes listing (`:2990` before `:3019`), so `tur run --list` in an untrusted tree executes shell. `find_justfile` walks up from cwd (`:1607-1625`). Recipe `{{ }}` args are unquoted into `system()` (`:2460`). Shebang recipes go to a hardcoded `/tmp/tur-run-XXXXXX` (`:2406-2439`). |
| D-3 | medium (under T1) | `tur repl` auto-discovery walks up from cwd (`src/turi/repl.c:1313-1325`), AOT-builds the tree and `dlopen`s the result; rebuild is decided by mtime, so a committed `.tur-repl-cache/lib-N.so` newer than the sources is `dlopen`ed without a build (`src/turi/spice_loader.c:713-723`). `TUR_NO_AUTO_SPICE=1` opts out. |
| D-4 | medium | `/tmp/tur-build` is `mkdir 0700` with the result ignored and no owner check (`main.c:2242-2253`); C paths are predictable and `fopen(..., "wb")` follows symlinks (`:2255-2273, :3184`); the prelude cache `<tur-build>/prelude/<hash>.o` is reused if non-empty with no integrity check (`:3034-3060`), so a planted object on a shared box is linked into the user's binary. |
| D-5 | medium | Spice destination is `spices/<name>-<ref>` with neither validated (`pkg.c:2531-2536`, `main.c:5720-5725`, `pkg.c:3088-3093`): `..` in a transitive manifest's name or ref escapes `spices/`. **verified** (`snprintf` at `pkg.c:2531-2536`). `:path` deps and `:members` are joined without a containment check (`pkg.c:3066-3110, 2161-2310`); `:build-dir` may be absolute. |
| D-6 | medium | Update-path fetch runs `git -C <dest> fetch --depth 1 origin <ref>` with no `--` before `<ref>` (`pkg.c:2046-2050`, **verified**), so a ref like `--upload-pack=<cmd>` from a transitive manifest is option injection; `tur install`'s `git ls-remote <url> <ref>` has the same shape (`src/compiler/install.c:1375`). The clone path is correct (`--branch <ref> -- <url>`). |
| D-7 | low | `TUR_SHQ` (`src/platform_fs.h:394-398`) is a bare `'` with no escaping; used for the run-binary path and pass-through argv (`main.c:5507-5513, 5572-5578, 6094-6098`), `tur fmt --diff` and `cmd_parse_check` file names (`:7832-7836, 8129-8132`), so a `'` in a path or argument breaks out. `open_in_browser` uses `xdg-open "%s"` (`:10498-10510`). |
| D-8 | low | MCP `execvp("tur", {format|build, <path>})` with no `--` (`src/lsp/mcp.c:232-247, 736, 758`); `try_external_subcommand` honours relative `PATH` entries (`main.c:11462-11508`). |
| D-9 | low | Generated `CMakeLists.txt` for `:cmake-deps` interpolates `:url`, `:ref`, `:options` unescaped (`pkg.c:3526-3569`), then `FetchContent` runs upstream CMake. |

### Memory safety and parsers (WP4, WP5)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| M-1 | high (under T2) | `tur_serial_cont_deserialize` has **no bounds checks**: frame count, name length, cstr length and env length are trusted from the bytes; raw int64s become frame environments; `__sk_frame_for_tag` takes an unchecked tag (`src/runtime/generated/tur_rt_split.c:2247-2291`; emitted copy `src/compiler/emit_dk_runtime.c:324`). `bytes->serial-cont` validates first but shallowly (`stdlib/serial.tur:598-651`); `resume-cont!` (`stdlib/workflow.tur:61-68`) and `image/blob-resume!` (`stdlib/image.tur:672-677`, via `load-resume-file!` `:699`) skip validation entirely. Image CRC covers the 68-byte header only, and `plen`/`goff` from the file size the `malloc` (`stdlib/image.tur:617-655`, `src/runtime/image.c:136-167`). The guestbook example resumes a continuation from a `POST` token -- this is the one place the project already ships T2 across a network. |
| M-2 | medium | JSON: input ending in `\` steps past the NUL terminator (compiled `stdlib/json.tur:545-563`; interpreter `src/turi/interpreter_natives.c:1211-1230`); no nesting depth limit (`json.tur:616-660`); no `\u`; error paths leak. |
| M-3 | medium (under T5) | LSP framing parses `Content-Length` with an unchecked `atol`; `-1` wraps `body_len + 1` to 0, `malloc(0)`, then a huge `read` -- heap overflow (`src/lsp/lsp_io.c:71-96`, **verified**); `read_headers` grows unbounded (`:40-68`). DAP reuses it (`src/turi/dap.c:818, 1114, 1340`). |
| M-4 | medium | `httpd`: `Content-Length` is `(int)strtol` into `malloc(content_len + 1)` with no cap (`stdlib/httpd.tur:293, 329, 2491`); `Transfer-Encoding` ignored (smuggling behind a proxy); static-file traversal guard is `strstr(path, "..")` with `stat` not `lstat` (`:4143-4200`); binds `INADDR_ANY` by default (`:720`). Multipart (`:2102-2176`) and Basic auth (`:1963, 2014`) unreviewed. |
| M-5 | medium | `read-async` does `malloc((size_t)bytes + 1)` with an unchecked, possibly negative `int` (`src/turi/fiber.c:763`); `tur_string_substring`/`slice` compute `start + len > n` with signed overflow (`src/runtime/tur_string.c:183, 300`); `n_from_bytes` accepts `len > strlen` (`src/turi/string_native.c:25-30`); `sb_reserve` doubles unchecked (`tur_string.c:238-243`); `bytes-alloc` `malloc(8 + (size_t)n)` with negative `n` (`stdlib/serial.tur:56-62`); `alloca(n * 8)` with user `n` in `stdlib/sized-buf.tur:445, 484` (gated `#fx{Unsafe}`). |
| M-6 | medium (silent UAF class) | Region escape hooks missing, per the CLAUDE.md rule: `tur_hamt_transient_set` (`src/runtime/hamt.c:1879`, from `stdlib/hamt.tur:740`), `tvar/write`/`tvar/swap` (`stdlib/stm.tur:88, 109`; `src/runtime/stm.c` has no note), `sized-buf-set!` (`sized-buf.tur:307`), `sized-matrix-set!` (`:232`), `sized-bitvec-set!` (`sized-bits.tur:139`), `httpd-resp-header-add!` (`httpd.tur:1281`). Each is a candidate use-after-rewind on the default build. |
| M-7 | info | The effect system is not a security boundary today: `--strict-effects` defaults off and only warns (`src/runtime/globals.c:135`); inline-C outside `Unsafe` is a lint behind `--lint-inline-c-unsafe`, default off (`globals.c:19`, `src/compiler/elab_toplevel.c:875`); the deserializers above infer plain rows. |

### Web (WP6)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| W-1 | medium | No CSP anywhere: no meta tag, none in `web/public/_headers` (COOP/COEP only) or `web/worker.js:78-92`. |
| W-2 | medium | `escapeHtml` (`web/main.js:905`) does not escape `"` but is used inside attributes (`main.js:1495, 1498-1499, 4733` -- `data-name="${escapeHtml(item.name)}"`); `escapeAttr` (`:922`) exists and is unused there. `hydrateConsole` re-inserts HTML from `localStorage['tur.try.console.v1']` unescaped (`:184-196`). |
| W-3 | low | No eval timeout or `worker.terminate` for the wasm worker (`web/public/eval-worker.js`), so a runaway program hangs its own tab. `#code=` share links only fill the editor (`main.js:1081-1090`) -- good. |
| W-4 | info | The docs pane `innerHTML`s same-origin docs-pack HTML (`main.js:5543`), so the trust boundary is `tools/gendocs.py` over `;;;` docstrings -- including third-party spice docstrings via `tools/genspices.py`. |

### Supply chain and CI (WP7)

| Id | Sev | Finding | Where |
| --- | --- | --- | --- |
| C-1 | high | Advertised installer is `curl -sSf https://turmeric-lang.com/install \| sh` (`README.md:20`, `web/index.html:331`) served by the Cloudflare Worker (`web/worker.js:1-24`), running `brew install --HEAD` against `main` (`Formula/turmeric.rb:5` is HEAD-only; no `url`/`sha256`). A compromised `main` -- or a bad afternoon on it -- ships to every new install. Release assets exist with `sha256sums.txt` (`.github/workflows/release.yml:349-352`) but nothing installs from them except `tvm`. |
| C-2 | medium | `tvm` checksum check fails open: skipped when `sha256sums.txt` is missing/empty, when the asset has no row, or when no sha tool exists (`tvm/tvm.sh:263-279`, **verified**); the sums file comes from the same origin as the asset, so it is integrity only. Base URLs are env-overridable (`TVM_RELEASE_BASE_URL`, `:205, 209`). |
| C-3 | medium | `tur.lock` is trust-on-first-use: the tree hash is recomputed and **overwritten** on every fetch, never compared (`src/compiler/pkg.c:2572-2582`); `:resolved` is never used to check out (clones track the branch/tag in `:ref`); verification happens only in `tur run` for already-present dirs, skipped for legacy/git-SHA hashes (`src/main.c:5710-5760`, `pkg.c:1744`). `consuming-spices-guide.md:405` overclaims. `tur audit` "lists; it does not verify" (`:420-424`). |
| C-4 | medium | Release assets are unsigned: no Sigstore/cosign, no GitHub build-provenance attestation, tags are `git tag -a` not `-s` (`.claude/commands/cut-*-release.md`). `softprops/action-gh-release@v2` runs floating with `contents: write` (`release.yml:338-355`). |
| C-5 | medium | No workflow pins an action to a SHA; `mymindstorm/setup-emsdk@v14` with `version: latest` (`ci.yml:134, 1470`); `msys2/setup-msys2@v2`; `pip install` unpinned (`ci.yml:105, 1467`; `release.yml:286`); `ci.yml:152-157` clones `turmeric-spices` default branch unpinned and compiles it; `ci.yml` has no top-level `permissions:` (default token scope everywhere except `publish-timings`' `contents: write`, `:762-765`); ccache `restore-keys` prefixes (`:112, 1454`). |
| C-6 | low | `fuzz.yml:75` interpolates `${{ inputs.seed }}` directly into a `run:` block (dispatch-only, so needs write access already; `inputs.n` at `:87` uses the safe `env:` form). Workflow has `issues: write` and `GITHUB_TOKEN` for `gh issue create` (`:41-43, 122+`). |
| C-7 | low | `cmake/mir.cmake:138-160` fetches MIR from the personal fork `rjungemann/mir.git` (SHA-pinned; JIT-only, default off). `examples/snake` pins raylib by tag. `Dockerfile` uses `ubuntu:22.04` by tag; `.devcontainer/Dockerfile` has two `curl \| bash` installs. |
| C-8 | low | Committed to git: `.claude/settings.local.json` (with a broad `Bash(xargs cat *)` allow), a `.claude/projects/.../memory/project_er6.md`, and `TEMP.md`. Missing: `SECURITY.md`, `CODEOWNERS`, `.github/dependabot.yml`, CodeQL/scanning workflow, and any private-vulnerability-reporting setting. |

## 3. Work packages

Each package names its scope, method, deliverable and exit criterion.
Effort is in focused engineer-days for someone who knows the tree; double
it for someone who does not. Findings are filed the usual way, under
`docs/reported/<slug>.md` with a row in the index -- **except** an
exploitable one, which goes to the private channel WP1 sets up until the
fix lands, then is archived normally.

### WP1 -- Threat model, SECURITY.md, disclosure channel (1 day)

- Turn section 1 into `docs/guides/security-guide.md` (the promises) and a
  root `SECURITY.md` (how to report, what is in scope, response time).
- Enable GitHub private vulnerability reporting on the repo; add
  `CODEOWNERS` for `src/turi/env.c`, `src/compiler/pkg.c`, `src/main.c`
  (driver), `stdlib/serial.tur`, `stdlib/image.tur`, `web/worker.js`,
  `.github/workflows/`.
- Correct the three overclaims in section 0 *now*, before their code is
  fixed: a doc that says "verified" while the code says TOFU is the finding
  a reporter writes up first.
- Remove `.claude/settings.local.json`, the memory file and `TEMP.md` from
  git and add them to `.gitignore` (C-8).
- **Exit:** the guide exists, links from README, and every later work
  package grades against it.

### WP2 -- Compiler driver: command construction and filesystem (4-5 days)

- **Method:** enumerate every `system(`/`popen(`/`execvp(`/`fork(` in
  `src/` (the survey's list is the starting inventory) and, per site, either
  (a) switch to `tur_shell_command`/`tur_shell_quote` for every interpolated
  piece, or (b) replace `system()` with `posix_spawn`/`execvp` on an argv.
  Prefer (b) for the compiler invocation: build an argv, never a string.
- Define what the autolink marker may carry (`-l<name>`, `-L<dir>`,
  `-framework X`, nothing else), parse it with that grammar instead of
  `strstr`, and reject the rest with a diagnostic naming the module (D-1).
- Validate spice `name`/`ref`, `:path`, `:members`, `:subdir` against a
  grammar (`[A-Za-z0-9._-]+`, no leading `-`, no `..`) at manifest-parse
  time, and add `--` before every positional git argument (D-5, D-6).
- Justfile: defer backtick evaluation until a recipe that uses the variable
  actually runs; `--list` must be pure (D-2). Honour `TMPDIR`. Quote `{{ }}`
  args or document that they are raw shell.
- `/tmp/tur-build`: create with `mkdir` + `lstat` owner/mode check (refuse a
  dir we do not own), `O_EXCL|O_NOFOLLOW` for the `.c` files, and either
  key the prelude cache by content hash *and* verify it, or move it under
  the project's `build/` (D-4).
- `tur repl` auto-discovery: require the `.tur-repl-cache` `.so` to be
  newer than *and* built by this `tur` (stamp it), or rebuild (D-3).
- Replace `TUR_SHQ` with `tur_shell_quote` everywhere (D-7); add `--` to
  the MCP argv (D-8); escape cmake interpolation (D-9).
- **Tests:** a fixture directory `tests/fixtures/security-driver/` with a
  manifest whose name is `a;id;`, a ref of `--upload-pack=touch x`, a
  `:c-sources` entry with a space and a quote, and an inline-C autolink of
  `-lfoo; touch pwned`; each must produce a diagnostic, never a file.
- **Exit:** `grep -n 'system(' src/` shows only argv-built or fully quoted
  sites, each with a one-line comment naming its quoting; the fixture is
  green.

### WP3 -- Interpreter sandbox capability audit (3 days)

- **Method:** dump the natives table (`turi_env_register_interpreter_natives`
  plus `collections_native.c`, `string_native.c`, `fiber.c`, the r7rs set,
  and `register_extern_c_known`) and classify every entry: pure / IO / FS /
  PROC / FFI / ENV / UNSAFE. The classification is the deliverable's core
  and lives in a table in the security guide.
- Enforce it at one choke point: give `turi_env_register_native` a required
  capability argument (or a parallel `native_caps[]` table) and have
  `eval_apply`'s native dispatch check `env->caps` against it. One check,
  not two hundred. `is_blocked_builtin` becomes the builtin half of the same
  table (S-1).
- Route `extern-c` known overrides through the same FFI check; make the
  interpreted `printf` a fixed-format that prints its arguments, never the
  program's format (S-2). Reject `%s`/`%n` in the inline-C snprintf emulator
  or run it only for `%d`/`%lld`/`%x` shapes (S-3).
- Build the PoC the survey could not: an `Env/new-sandboxed` embedder
  program (`stdlib/turi/eval.tur`) that evaluates `(process/spawn ...)`,
  `(io-fopen-write ...)`, `(read-async 0 -1)`; then the macro-time route
  (`:for-macros` on a module whose function calls a native by its bare
  name) -- if it is genuinely unreachable, write down *why* so the
  reasoning survives the next refactor.
- Extend `tests/turi/sandbox-eval.c` to assert every non-pure native is
  denied under `CAP_NONE`, generated from the classification table so a
  new native cannot be added unclassified.
- Decide and document the Try Turmeric posture (S-4): `CAP_ALL` behind the
  browser sandbox is fine; say so in the security guide, and note that the
  wasm build has no filesystem or process anyway.
- **Exit:** the generated sandbox test is green; the sandboxing guide's
  claims match the table.

### WP4 -- Deserializers, parsers, and the fuzz harnesses (5-6 days)

This is the package with the most lasting value: it leaves libFuzzer
targets behind that run under the ASan build CI already has.

- **Serial continuations and images (M-1):** make `tur_serial_cont_deserialize`
  bounds-check every length against the remaining input, validate every
  tag against the registry before use, and cap frame count and env size;
  route `resume-cont!` and `image/blob-resume!` through `bytes->serial-cont`'s
  validation (or make the validation live inside the deserializer so there
  is no unvalidated entry). Extend the image CRC over the payload, or add a
  payload hash to the header. Make the guestbook's `POST /submit?k=TOKEN`
  the worked example: today a forged token is a forged continuation.
  Decide (section 7) whether continuations from the network need an HMAC
  -- integrity is not authenticity.
- **JSON (M-2):** fix the trailing-backslash over-read in both parsers, add
  a depth limit (256 is plenty), decide on `\u`, free on error.
- **LSP/DAP framing (M-3):** `strtoul` with range check, reject
  `Content-Length` over a sane cap (16 MiB), cap header bytes.
- **httpd (M-4):** cap body size (configurable, default a few MiB), parse
  `Content-Length` as `size_t` with overflow check, reject requests with
  both `Content-Length` and `Transfer-Encoding` (or implement chunked),
  `lstat`+`realpath` containment for static files, default bind to
  `127.0.0.1` with an explicit opt-in for `0.0.0.0`; review multipart and
  Basic auth.
- **Harnesses:** add `tests/fuzz/` with libFuzzer targets for
  `serial_cont_from_bytes`, `tur_serial_cont_deserialize`, the image header
  reader, both JSON parsers, `lsp_read_message`, `httpd`'s request parser,
  and -- the compiler's own front door -- the reader (`src/compiler/reader.c`),
  `pkg_manifest_read`, and the Justfile parser. Build them under
  `-fsanitize=fuzzer,address,undefined` behind a `TUR_FUZZ` CMake option;
  seed each from the fixture corpus. Add a `fuzz-parsers` job to
  `fuzz.yml` running each target for a fixed budget nightly, alongside the
  existing differential fuzzers, and file crashes the way that workflow
  already files issues.
- **Exit:** every target runs 10 minutes clean under ASan; the fixed bugs
  each have a fixture with the crashing input.

### WP5 -- Runtime memory safety (3 days)

- **Region escape hooks (M-6):** walk every `-set!`/`-push!`/insert/store
  primitive in `stdlib/` and `src/runtime/` against the hooked list in
  CLAUDE.md; note the missing ones; add each to
  `tests/fixtures/region-escape-via-store`. The rule says a missed hook is a
  silent use-after-rewind, so this is not a style pass.
- **Integer arithmetic on sizes (M-5):** `read-async`, string slicing,
  `sb_reserve`, `bytes-alloc`, `alloca` in sized-buf: use `size_t`, check
  `start + len` with `__builtin_add_overflow`, and reject negative counts
  at the boundary. Grep `malloc(.*\*` and `alloca(` across `src/runtime`,
  `src/turi`, `stdlib` for the rest.
- **TSan:** the `TSan` CMake config exists and no workflow uses it; add a
  nightly job over the STM/async/fiber fixtures (`requires.tsan` already
  marks them).
- **Format strings:** confirm the survey's "checked and sized" list, and
  add `-Wformat=2 -Wformat-security` to the Debug flags so the compiler
  polices new ones.
- **Exit:** the region fixture covers every setter; TSan job green;
  `-Wformat-security` clean.

### WP6 -- Try Turmeric and the web worker (1-2 days)

- Add a CSP to `web/public/_headers` and the worker (`default-src 'self'`,
  `script-src 'self' 'wasm-unsafe-eval'`, `worker-src 'self'`,
  `connect-src 'self' https://raw.githubusercontent.com`, `style-src` as
  the fonts need, `object-src 'none'`, `frame-ancestors 'none'`) and fix
  whatever it breaks (W-1).
- Use `escapeAttr` in attribute contexts; make `escapeHtml` escape `"` and
  `'` anyway; re-escape or store structured data instead of HTML in the
  console persistence key (W-2).
- Give the eval worker a watchdog and a "Stop" that terminates and
  re-creates it (W-3). Document W-4's boundary (docstrings are trusted
  content from this repo and `turmeric-spices`).
- **Exit:** Playwright suite green with CSP enforced; an attribute-injection
  test in the suite.

### WP7 -- Supply chain, release, CI (3 days)

- **Installer (C-1):** make `/install` fetch the latest *release* tarball
  and verify `sha256sums.txt` against a value pinned *in the script* for
  that release (or, better, make `tvm` the installer and have `/install`
  bootstrap `tvm`); give `Formula/turmeric.rb` a stable `url`+`sha256`
  block with `head` as the opt-in; update `README.md:20` and
  `web/index.html:331`.
- **`tvm` (C-2):** fail closed -- missing sums, missing row, or missing
  sha tool is an error unless `--insecure` is passed; pin the release base
  URL unless an env override is explicitly acknowledged.
- **Lock verification (C-3):** compare the fetched tree hash with the lock
  row and fail with a diagnostic on mismatch unless `tur fetch --update`;
  check out `:resolved` when present rather than tracking `:ref`; run the
  check in `tur build` as well as `tur run`; make `tur audit` verify. Then
  fix the guide to say what the code does.
- **Release (C-4):** `actions/attest-build-provenance` on every asset,
  `git tag -s` in the cut-release commands (or document the key decision),
  pin `softprops/action-gh-release` to a SHA.
- **Workflows (C-5, C-6):** pin every action to a full SHA with a version
  comment (Dependabot can keep them fresh), add a top-level
  `permissions: contents: read` to `ci.yml` and `release.yml` and raise it
  per job, pin `pip` requirements with hashes, pin the `turmeric-spices`
  clone to a SHA the repo records, move `inputs.seed` to `env:`, set
  `version:` on `setup-emsdk`. Add `.github/dependabot.yml` for actions,
  npm (`web/`), and pip; add a CodeQL workflow for C and JavaScript.
- **Exit:** every action SHA-pinned; release assets carry attestations; the
  installer verifies a checksum; the lock check has a fixture.

### WP8 -- Effects as a stated boundary (1 day, decision-heavy)

- Decide what `#fx{Unsafe}` promises (section 7). If it is meant as a
  trust marker -- "this function can corrupt memory on bad input" -- then
  the deserializers in M-1/M-5 need it, and the survey's list is the
  backlog. If it is purely about pointer arithmetic, say so, and the
  security guide stops mentioning it.
- Consider defaulting `--lint-inline-c-unsafe` on once `stdlib/` is clean
  (a follow-on to `docs/reported/stdlib-int-stand-in-audit.md`'s sweep).
- **Exit:** one paragraph in the security guide, and a decision recorded
  in this plan's status line.

## 4. Sequencing

| Week | Packages | Why this order |
| --- | --- | --- |
| 1 | WP1, WP7 (installer, tvm, workflow pins, permissions), WP4 (M-3 LSP framing; M-1 bounds checks) | The cheapest changes with the largest blast radius: what a user installs, what CI can do with its token, and the two overflows a reporter would demo first. |
| 2 | WP2 (D-1, D-2, D-5, D-6), WP3 (S-1 choke point) | The compiler-driver injection class and the sandbox bypass. Both are one design change each plus a sweep. |
| 3 | WP4 (harnesses, JSON, httpd), WP5 | The fuzz targets need WP4's fixes landed to seed sensibly; region hooks and integer checks are independent. |
| 4 | WP6, WP8, WP2/WP3 remainder, re-grade section 2 | Web hardening, the effects decision, and closing the long tail. |

Roughly 22-25 engineer-days of focused work, spread over four weeks with
the fuzzers running nightly from week 3. Nothing here blocks the v1
track: every package is independently landable, and the plan is a
checklist, not a gate.

## 5. Method and tooling

- **Static:** the grep inventories in section 2 (subprocess, `getenv`,
  `malloc(.*\*`, `alloca`, `innerHTML`), plus `clang-tidy` with the
  `cert-*`, `bugprone-*` and `security.*` checkers over `src/` and CodeQL
  in CI (WP7). `-Wformat=2 -Wformat-security -Wshadow` on Debug.
- **Dynamic:** the Debug build's ASan+UBSan (already on, leak detection on
  for the compiler path), the new libFuzzer targets (WP4), the TSan job
  (WP5), `tests/run-leak-check.sh` for emitted programs.
- **Manual review checklist** for each boundary crossing: who supplies
  each byte, every length used for allocation or indexing is checked
  against the remaining input, every string that reaches a shell is
  quoted or is an argv element, every path is canonicalised and contained,
  every native declares its capability.
- **Filing:** one `docs/reported/<slug>.md` per defect with the usual
  repro/root-cause/fix-direction shape and a `security-` tag in the index
  row; exploitable ones through the private channel first (WP1).

## 6. Definition of done

1. The security guide and `SECURITY.md` exist and the three overclaims are
   corrected.
2. Every section 2 row is either fixed with a fixture, filed as an open
   report, or retired with a one-line reason in this plan.
3. `tests/fuzz/` targets run nightly under ASan and file crashes.
4. The generated sandbox test pins every native's capability.
5. Release assets are attested and the installer verifies a checksum.
6. Every workflow action is SHA-pinned with least-privilege permissions.

## 7. Open questions for the author

1. **T1 line:** is `tur check` / the LSP / `tur run --list` / `tur repl`
   auto-discovery meant to be safe on a tree you have merely opened? The
   plan assumes yes (editors run them without asking).
2. **Continuations over the network:** should `bytes->serial-cont` accept a
   key and verify an HMAC, or is it the application's job (the guestbook
   example would then need to show it)?
3. **`#fx{Unsafe}` semantics** (WP8): pointer arithmetic only, or "may
   corrupt memory on bad input"?
4. **Try Turmeric:** keep `CAP_ALL` behind the browser sandbox (proposed),
   or run the wasm env sandboxed too for defence in depth?
5. **Installer:** keep Homebrew as the primary channel (with a stable
   formula), or make `tvm` the advertised path?
6. **Release signing key:** Sigstore keyless via GitHub OIDC (no key to
   manage; proposed), or a maintainer GPG key?
7. **Who** runs the audit -- one person over four weeks, or the packages
   handed out? WP2 and WP3 want someone who knows the driver and the
   interpreter respectively; WP4 and WP7 do not.
