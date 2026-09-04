# Try Turmeric: LSP-backed editor intelligence

> **Status:** Executed (2026-07-29) -- L0-L4 landed. One item cannot be closed
> in the execution environment (no `emcc`); see [§6 Execution record](#6-execution-record).
> **Type:** Web playground / `src/lsp` / WASM glue
> **Related:** [`lsp-client-gaps-plan.md`](lsp-client-gaps-plan.md) (executed),
> [`try-turmeric-lang-toggle-plan.md`](try-turmeric-lang-toggle-plan.md)

## 0. Summary

Try Turmeric today gives Monaco a Monarch tokenizer (`web/main.js:980`), a
bracket configuration (`main.js:1017`), and a single document formatter wired
to `turi_wasm_format` (`main.js:1401`). Everything else -- completion, hover,
diagnostics, go-to-definition, document symbols, signature help -- is absent.
Meanwhile `tur lsp` already implements every one of those, and after the
`lsp-client-gaps-plan` execution it does so at a quality a real client
(Trowel) shipped against.

**The whole server is already inside the WASM bundle's source set.** The
`lsp/*.c` files are members of the same core source list that feeds
`WASM_SOURCES` (`src/CMakeLists.txt:255-261` and `:1180`). What is missing is
not an implementation -- it is a *transport*. `lsp_server_run(fd_in, fd_out)`
(`src/lsp/lsp.c:1449`) is a blocking `read(2)`/`write(2)` loop, and a browser
has neither.

So this plan is: **split transport from dispatch**, export a
one-message-in / messages-out entry point to JS, and write a thin Monaco
adapter. No new language intelligence is written at all.

---

## 1. What exists (repo facts)

### 1.1 Server surface, already shipping

`lsp_server_run` dispatches (`src/lsp/lsp.c:1481-1546`):

| Method | Handler |
|---|---|
| `initialize` / `initialized` / `shutdown` / `exit` | `on_initialize`, `on_shutdown` |
| `textDocument/didOpen` / `didChange` / `didClose` | `on_did_open`, `on_did_change`, `on_did_close` |
| `textDocument/publishDiagnostics` (server->client) | `lsp_flush_dirty`, `src/lsp/lsp.c:443` |
| `textDocument/formatting` | `on_formatting` (in-process, shared with `tur fmt`) |
| `textDocument/hover` | `on_hover` |
| `textDocument/definition` | `on_definition` |
| `textDocument/documentSymbol` | `on_document_symbol` |
| `workspace/symbol` | `on_workspace_symbol` |
| `textDocument/signatureHelp` | `on_signature_help` |
| `textDocument/completion` | `on_completion` (emits a `CompletionList`) |
| `$/cancelRequest` | cancel queue |

Analysis is a real compile: the document text is written to a temp file under
`tur_temp_dir()` and fed to `tur_collect_symbols` (`src/lsp/lsp.c:346-372`),
with a last-good symbol index retained so completion survives a broken buffer,
plus an empty-file stdlib prime (`stdlib_cache_prime`, `src/lsp/lsp.c:293`).

### 1.2 Two properties of the browser make this *easier* than the native case

- **`workspace/symbol` covers open documents only** -- recorded as an
  unscheduled gap (`lsp-client-gaps-plan.md` §3.5). In Try Turmeric every tab
  in the workspace *is* an open document, so the gap does not exist here.
- **No filesystem to disagree about.** The path-based analysis note in §4 of
  that plan (client and server arguing over URI-vs-path) collapses: the
  playground mints its own `file:///project/<tab>.tur` URIs and nothing else
  ever reads them.

### 1.3 The WASM bundle

`add_custom_target(tur_wasm ...)` (`src/CMakeLists.txt:1195`) compiles
`WASM_SOURCES` with `emcc -O2`, bundles `stdlib/` into the virtual FS at
`/stdlib`, and exports a fixed function list (`:1222`). `wasm-ld` garbage
collects unreferenced code, and nothing currently references
`lsp_server_run`, so the LSP handlers are compiled but almost certainly
stripped from today's `turmeric.wasm`. **Expect a bundle-size increase when
they are retained** -- measure it in Phase L1 (§5) and record the number.

JS side: `web/main.js:793` spawns `/eval-worker.js`, a Worker holding one
`TurmericModule()` instance; `main.js` posts `{type:'eval'|'reset'}` and the
worker replies by id. Evaluation is serialized through an `executionQueue`
(`main.js:913-941`).

---

## 2. Design

### 2.1 Transport/dispatch split (`src/lsp/lsp_session.c`)

Today every handler takes `int fd_out` and calls `send_response(fd_out, ...)`.
Introduce a sink:

```c
/* src/lsp/lsp_sink.h */
typedef struct LspSink LspSink;
LspSink *lsp_sink_fd(int fd);       /* framed Content-Length write(2) */
LspSink *lsp_sink_buf(Buf *out);    /* append framed messages to a buffer */
void     lsp_sink_send(LspSink *, const char *json, size_t len);
```

Mechanical change: every `int fd_out` parameter in `src/lsp/lsp.c` becomes
`LspSink *sink`, and `lsp_write_message` (`src/lsp/lsp_io.c`) grows a
buffer-backed sibling. No handler logic moves.

Then the dispatch body of `lsp_server_run` (the `if (strcmp(method, ...))`
chain, `lsp.c:1481-1546`) is lifted verbatim into:

```c
/* Dispatch exactly one JSON-RPC message. Appends zero or more framed
 * messages (the response, plus any publishDiagnostics notifications the
 * handler triggers) to `out`. Returns false on `exit`. */
bool lsp_session_handle(const char *msg, size_t len, Buf *out);
```

`lsp_server_run` becomes a stdio loop calling it. **The native binary's
behaviour must not change** -- that is what makes this refactor safe to land
ahead of any browser work.

### 2.2 Transport-specific behaviour behind a capability flag

Three things in the loop are inherently stdio:

- the debounce (`input_pending(fd_in, LSP_ANALYSIS_DEBOUNCE_MS)`, `lsp.c:1457`)
- `drain_pending(fd_in)` before a slow request (`lsp.c:1526`)
- `_exit()` on `exit` (`lsp.c:1495`)

In the browser, **the client owns the debounce** (Monaco's own event timing)
and there is no socket to peek. So `lsp_session_handle` takes the
already-drained view: no peeking, and a `dirty` document is flushed
synchronously at the head of any request that reads `doc->symbols`. On `exit`,
free and return `false` rather than `_exit`.

Practical consequence to state plainly: **completion in the browser is
synchronous with a compile.** That is the same cost `tur lsp` pays; the
difference is that the browser cannot hide it behind a quiet-typing window
unless JS supplies one. The JS adapter therefore carries a 150 ms trailing
debounce on `didChange` (§2.5).

### 2.3 WASM export

```c
/* src/web/wasm_glue.c */
char *turi_wasm_lsp_request(const char *json);  /* caller frees */
void  turi_wasm_lsp_reset(void);
```

Returns a JSON array of the messages the server produced --
`[{...response...},{...publishDiagnostics...}]` -- rather than
`Content-Length`-framed bytes, because the JS side has no reason to parse
framing it is about to discard. Add both to `-sEXPORTED_FUNCTIONS`
(`src/CMakeLists.txt:1222`).

Open items to verify in Phase L0, not to assume:

- **`mkstemps` under Emscripten.** `stdlib_cache_prime` and the analysis path
  both use it (`lsp.c:302`, `:353`). MEMFS provides `/tmp`, but confirm
  `tur_temp_dir()` returns something writable in the browser and that
  `mkstemps` (the 4-suffix variant, for the `.tur` extension) links.
  Fallback if not: a fixed `/tmp/tur_lsp_<counter>.tur` under an
  `#ifdef __EMSCRIPTEN__`.
- **stdlib discovery.** `stdlib_root()` must resolve to the bundled `/stdlib`
  so `stdlib_cache_prime` yields the full surface. The eval path already
  relies on that bundle, so this is likely free.
- **Diagnostics need `#lang` handling.** The analysis path writes the raw
  document to a `.tur` temp file; verify `tur_collect_symbols` runs
  `detect_lang_layered` so a `#lang turmeric/sweet` buffer is not reported as
  a wall of syntax errors. This is the seam shared with the language-toggle
  plan; if it does not, that fix belongs here, in L1.

### 2.4 Worker topology -- a second Worker

**Recommendation: a dedicated `/lsp-worker.js` with its own `TurmericModule()`
instance, separate from `/eval-worker.js`.**

Rationale: eval and analysis must not queue behind each other. A user running
a 3-second loop should still get completion; conversely `turi_wasm_reset`
(`main.js:945`) tears the eval environment down to prelude, which an LSP
sharing the instance would notice. Separate instances give separate linear
memories and no shared mutable global state.

Cost: a second wasm instantiation (roughly doubles playground wasm memory) and
a second module fetch -- though the same `turmeric.wasm` URL, so it is a cache
hit, not a second download. Mitigate by instantiating the LSP worker **lazily**
on first editor focus rather than at page load, so the time-to-first-paint and
the mobile memory profile are unchanged for a user who only reads code.

If Phase L2 measurement shows the memory cost is unacceptable on mobile
Safari, the documented fallback is one shared worker with a strict priority
queue (LSP requests jump ahead of queued evals, never preempt a running one).

### 2.5 Monaco adapter (`web/lsp-client.js`)

**Do not pull in `monaco-languageclient`.** It drags in `vscode-jsonrpc`,
`vscode-languageclient`, and a `vscode` shim -- a large dependency to bridge
six methods whose Monaco-side provider APIs are each a dozen lines. Hand-roll
a ~300-line adapter instead; revisit only if the method count grows past
roughly a dozen.

The adapter owns:

- **Lifecycle.** `initialize` on worker ready; one `didOpen` per tab; `didClose`
  on tab close; `didChange` on model content change, trailing-debounced 150 ms.
  Tabs already carry Monaco models (`main.js:245`), so the URI is
  `file:///project/${tab.name}`.
- **Providers.** `registerCompletionItemProvider` (trigger `"("`, honouring
  `isIncomplete`), `registerHoverProvider`, `registerSignatureHelpProvider`,
  `registerDefinitionProvider`, `registerDocumentSymbolProvider`.
- **Diagnostics.** `publishDiagnostics` notifications route to
  `monaco.editor.setModelMarkers(model, 'turmeric', markers)`. Severity maps
  1:1; the zero-width-range widening already happened server-side
  (`diag_lsp_flush_array`).
- **Formatting.** Keep `turi_wasm_format` as the formatter for now -- it is
  the same in-process implementation `on_formatting` calls, and it already
  works. Route through the LSP only if the LSP worker is up anyway; do not
  make formatting depend on the LSP worker booting.
- **Degradation.** If the LSP worker fails to instantiate, every provider
  returns empty and diagnostics stay clear. The playground must never become
  *less* usable than it is today because of an analysis failure.

### 2.6 What is explicitly out of scope

- Semantic tokens (`lsp-client-gaps-plan.md` §3.4, unscheduled). Monaco keeps
  the Monarch tokenizer.
- Rename, code actions, inlay hints -- the server has none.
- Multi-root workspaces. One project, one flat tab set.

---

## 3. Phases

- **L0 -- feasibility spike (timeboxed).** Build `tur_wasm` with
  `_turi_wasm_lsp_request` exported behind a throwaway `lsp_session_handle`,
  and from the browser console drive `initialize` -> `didOpen` -> `completion`
  on a 5-line document. Exit criteria: real completion items come back, and
  the `mkstemps`/`stdlib_root`/`#lang` questions in §2.3 are each answered
  yes-or-fixed. Record the `turmeric.wasm` size delta. If temp-file analysis
  cannot work in MEMFS at all, stop and re-scope to a symbols-only subset
  (completion/hover from the stdlib cache, no diagnostics).
- **L1 -- transport split.** §2.1 and §2.2 landed properly, native behaviour
  unchanged, `tests/lsp/run-mcp-lsp.sh` still green. Ships independently of
  any web change and is worth having on its own.
- **L2 -- worker + diagnostics.** `web/lsp-worker.js`, `web/lsp-client.js`,
  lazy instantiation, `publishDiagnostics` -> Monaco markers. Diagnostics
  first: it is the highest-value surface and the one that needs no UI work.
- **L3 -- providers.** Completion, hover, signature help, document symbols,
  definition. Definition is cross-tab -- it must switch the active tab when
  the target URI is another one.
- **L4 -- polish.** Cancellation on rapid typing (`$/cancelRequest` is
  already implemented server-side), a status indicator when analysis is in
  flight, and mobile behaviour (completion popovers on a phone keyboard need
  checking, not assuming).

---

## 4. Testing

- **Native regression** -- `bash tests/lsp/run-mcp-lsp.sh` after L1. The
  refactor is only safe if this is untouched.
- **Session-level unit tests** -- a small C harness feeding scripted messages
  to `lsp_session_handle` and asserting on the emitted array. This is
  transport-free, so it is the cheapest place to pin behaviour and it covers
  the browser path without a browser.
- **Playwright** -- `web/tests` already exists with a Playwright config. Add:
  type an undefined symbol and assert a marker appears; type `(cons` and
  assert signature help; `(co` + Ctrl+Space and assert completion items.
- **Twelve-minute timeout rule applies** to every suite invocation, as always.

---

## 5. Risks

| Risk | Mitigation |
|---|---|
| Bundle size grows materially | Measure in L0. If it is bad, an `-sEXPORTED_FUNCTIONS`-driven second wasm artifact (eval-only vs eval+lsp) is possible but is a real complexity cost -- prefer paying the bytes. |
| Analysis-per-keystroke is too slow in wasm | 150 ms trailing debounce, plus the server's own last-good index means a slow analysis degrades to stale-but-useful rather than empty. |
| Two wasm instances exhaust mobile memory | Lazy instantiation on editor focus; documented single-worker fallback in §2.4. |
| MEMFS temp files unusable | Detected in L0, before any web work is written. |

---

## 6. Execution record

Written after the fact. The plan body above is left as it was proposed; this
section records what actually happened, including where the plan was wrong.

### 6.1 Where the plan's repo facts were wrong

**"The whole server is already inside the WASM bundle's source set" (§0) was
nearly true, and the gap was a hard blocker.** `lsp/*.c` are in
`TUR_CORE_SOURCES`, but `tur_collect_symbols` -- the analysis every handler
calls -- is defined in `src/main.c`, which is a CLI and is *not* in the WASM
source set. Nothing referenced `lsp_server_run`, so `wasm-ld` collected the
whole server away and the dangling reference never surfaced. Exporting an entry
point makes it live, and the link would have failed on the first `emcc` run.

Confirmed statically before writing any code: `strings web/public/turmeric.wasm
| grep -c textDocument` is `0`, so the handlers really are absent from today's
bundle, exactly as §1.3 predicted -- and so is the analysis they call.

The fix is `src/web/wasm_lsp.c`, which supplies a WASM-side
`tur_collect_symbols` over pieces now shared with `main.c`
(`compiler/stdlib_autoload.c`, `lsp/lsp_collect.c`). It stops after
elaboration rather than running the whole `compile_to_c` pipeline: names,
types, spans, and type errors all exist by then, and the later passes exist to
produce a binary. **Stated cost:** borrow-check and lifetime errors do not
appear as markers in the playground. The Run button still reports them.

**The buffer sink emits a JSON array, not framed messages.** §2.1 specified
`lsp_sink_buf` as "append framed messages to a buffer" while §2.3 specified the
export as returning a JSON array "because the JS side has no reason to parse
framing it is about to discard". Those contradict. The array won: the sink
appends array elements and `lsp_sink_buf_open/close` bracket them, so nothing
frames anything only to strip it back off.

**`#lang` handling needed no fix.** §2.3 flagged it as a risk. `compile_to_c`
already runs `detect_lang_layered`, and the WASM front end does the same;
`tests/lsp/wasm_backend_test.c` asserts a `#lang turmeric/sweet` buffer -- which
is what Try Turmeric opens with -- analyses clean.

**`stdlib_root()` needed no fix either.** It reads `TUR_STDLIB_DIR`, which
`wasm_lsp_init()` sets to the `/stdlib` mount point.

### 6.2 Defects found while executing

- **`stdlib_cache_prime` latched per process, not per session**
  (`src/lsp/lsp.c`). It used a function-local `static int attempted`, which on
  stdio is indistinguishable from correct -- one session is one process there.
  A browser module outlives its session: the latch stayed set while
  `stdlib_cache_free()` dropped the cache it guarded, so from the second
  session onward a buffer opened with a syntax error already in it had
  completion dead for the life of the page. That is precisely the case the
  fallback exists for. Fixed; regression test resets and re-checks.
- **`.status-indicator { display: flex }` beat `[hidden]`** (`web/styles.css`).
  A class rule outranks the user agent's `[hidden] { display: none }`, so the
  analysis indicator stayed visible for every visitor whose server never
  booted. Caught by the degradation spec on its first run.
- **Compiler-internal symbols flood completion** -- reported, not fixed:
  [`docs/archive/lsp-completion-internal-symbols.md`](lsp-completion-internal-symbols.md).
  Elaborator-synthesised globals (`__inst_Eq_eq_qu_int`, `__fn_774`) are
  collected as ordinary symbols and overrun the 200-item cap on a trivial
  buffer, so the stdlib names a user actually wants are the ones cut. Native
  `tur lsp` has always had this; it is not new here.

### 6.3 What landed

| Phase | Landed as |
|---|---|
| L1 transport split | `src/lsp/lsp_sink.{h,c}`, `src/lsp/lsp_session.{h,c}`, dispatch lifted out of `lsp_server_run` |
| L0 WASM bridge | `src/web/wasm_lsp.c`: `turi_wasm_lsp_request` / `_flush` / `_reset` + the analysis backend |
| L2 worker + diagnostics | `web/public/lsp-worker.js`, `web/lsp-client.js`, markers via `setModelMarkers` |
| L3 providers | completion, hover, signature help, definition (cross-tab), document symbols |
| L4 polish | analysis indicator in the editor footer; client-side cancellation |

Shared extractions that made the second front end possible without a copy:
`src/lsp/lsp_collect.c` (the binding walk) and
`src/compiler/stdlib_autoload.c` (the autoload list + form prepend, with the
stdlib directory now a parameter).

### 6.4 Testing, and what it does not cover

| Suite | Result |
|---|---|
| `tests/lsp/run-mcp-lsp.sh` (native regression, §4) | 61/61 -- unchanged by the refactor |
| `tests/lsp/session_test.c` (`tur_lsp_session_unit`) | 50 assertions, transport-free |
| `tests/lsp/wasm_backend_test.c` (`tur_lsp_wasm_backend_unit`) | 19 assertions, the browser backend run natively |
| `bash tests/run.sh` | 2411/2411 |
| `web/tests/lsp.spec.js` | 10 passing, 5 skipped |

`wasm_backend_test.c` deserves a note. `src/web/wasm_lsp.c` would otherwise
only ever execute inside a wasm module, where a mistake surfaces months later
as "completion in the browser is empty". Nothing in it is actually
wasm-specific -- the Emscripten build differs only in where the stdlib is
mounted, and that is a `TUR_STDLIB_DIR` lookup -- so it runs against the
in-tree stdlib and the real compiler in CI.

The 5 skipped Playwright specs are the ones that need a bundle with the LSP
exports; `web/public/turmeric.wasm` predates them. Rather than ship the browser
half with no coverage until someone runs `emcc`, a third `describe` intercepts
the worker script and serves a scripted server speaking the same protocol, with
answers copied from what the C tests assert the real one produces. The real
client drives the real Monaco providers; only the language server is a script.

**Not covered, and not coverable without `emcc`:**

- **The wasm link itself.** `src/web/wasm_lsp.c` compiles clean natively (it is
  in `WASM_GLUE_SOURCES`, so the `libturi_wasm` target builds it every time),
  but nothing here has run `wasm-ld` over it.
- **`mkstemps` under Emscripten** (§2.3). `lsp.c` uses the 4-suffix variant for
  its analysis temp file. Emscripten's musl-derived libc provides it, so no
  `#ifdef __EMSCRIPTEN__` fallback was written; if that is wrong the link fails
  loudly rather than silently, which is the acceptable failure mode.
- **`/tmp` in MEMFS.** `tur_temp_dir()` returns `/tmp`, which Emscripten's
  default FS creates. Unverified.
- **The bundle-size delta** (§5, and an explicit L0 exit criterion). Cannot be
  measured without a build. Retaining the LSP handlers plus the elaborator
  front end will not be free.
- **Mobile behaviour** (L4). Completion popovers on a phone keyboard needed
  checking rather than assuming, and the mobile Playwright project could not
  launch a browser in this environment.

## 7. Closing the `emcc` gap

Everything §6.4 listed as uncoverable is now covered. Built with Homebrew
Emscripten 5.0.5 on macOS arm64:

```sh
cmake -S . -B build-wasm -DTUR_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target tur_wasm
```

### 7.1 The link, and the two guesses it settled

`wasm-ld` linked clean on the first run, and all three exports are live in the
module -- `wasm-objdump -x -j export` shows `turi_wasm_lsp_request`,
`turi_wasm_lsp_flush`, `turi_wasm_lsp_reset`. `strings turmeric.wasm` now finds
`textDocument/publishDiagnostics` and the full `initialize` capabilities blob,
against `0` matches before, so the handlers §1.3 predicted were being collected
away really are retained now.

**`mkstemps` was the right guess.** It resolved from Emscripten's libc with no
`#ifdef __EMSCRIPTEN__` fallback needed. **`/tmp` in MEMFS was too**, and that
one the link could not have shown: the analysis path writes the buffer to a
temp file and compiles it, so every diagnostic that reaches a Monaco marker in
the browser is a round trip through `tur_temp_dir()` that completed. The
provider specs below are what prove it.

### 7.2 Bundle-size delta -- the L0 exit criterion

| Artifact | Before | After | Delta |
|---|---|---|---|
| `turmeric.wasm` | 3,029,099 | 3,060,041 | **+30,942 (+1.0%)** |
| `turmeric.js` | 84,085 | 85,930 | +1,845 (+2.2%) |

§5 budgeted for this being "not free" and offered a second eval-only artifact
if it went badly. It did not: 30 KB on a 3 MB module, gzipped rather less. The
reason the number is this small is worth stating, because it is not that the
LSP is small -- it is that the expensive half was already there. The
elaborator, the type checker, and the stdlib are linked in for the eval path
regardless; what `wasm-ld` newly retains is the handler dispatch and its JSON
plumbing. **No second artifact. Pay the bytes**, exactly as §5 preferred.

### 7.3 A defect the previous environment could not have found

`tur_lsp_session_unit` and `tur_lsp_wasm_backend_unit` both link
`$<TARGET_OBJECTS:tur_core>`, which carries `repl.c`, and neither carried the
`if(HAVE_EDITLINE)` block every other `tur_core`-linking target in
`src/CMakeLists.txt` has. That fails exactly backwards: it links clean on a box
where CMake found no libedit and fails with seven undefined `readline` symbols
on one where it did. The environment that wrote these targets was the former,
so both looked fine. Fixed by adding the block to each.

### 7.4 Mobile, checked rather than assumed

L4 wanted "completion popovers on a phone keyboard checked, not assumed", and
the previous run could not launch the mobile project at all. With WebKit
installed the `mobile` project (iPhone 13, 390x664) runs, and
`web/tests/mobile.lsp.spec.js` pins the two things that are phone-specific:

- **Lazy instantiation actually holds.** A visitor who loads the page, runs the
  buffer, and reads the output never instantiates the second module --
  asserted on the device where the second 64 MB heap is the whole reason §2.4
  chose lazy. This is the memory mitigation, made falsifiable.
- **The suggest widget stays inside the viewport.** Monaco positions it against
  available space, and on a 390px-wide screen a list rendered past the edge is
  indistinguishable from no completion at all. The spec asserts the widget's
  bounding box against the viewport on all four sides.

Diagnostics and boot-on-WebKit are covered too; the degradation contract from
§2.5 is re-asserted mobile-side, since a phone that cannot run the server must
still show no fault.

The single-worker fallback §2.4 documented is **not needed**: two instances
boot and run on mobile WebKit without trouble.

### 7.5 Results

| Suite | Result |
|---|---|
| `tests/lsp/run-mcp-lsp.sh` | 61/61 |
| `tur_lsp_session_unit` | 50/50 |
| `tur_lsp_wasm_backend_unit` | 19/19 |
| `bash tests/run.sh` | 2411/2411 |
| `web/tests/lsp.spec.js` (desktop) | **15 passing, 0 skipped** (was 10 + 5 skipped) |
| `web/tests/mobile.lsp.spec.js` (WebKit) | 4 passing |

The 5 previously-skipped specs now run against the real server: markers appear
and clear, completion offers the buffer's own definitions through the real
suggest widget, hover reports a type, and every tab is an open document.

Pre-existing failures elsewhere in `web/tests`, confirmed unrelated by
re-running them against the previous bundle: `guide-toggle.spec.js` and
`prod-smoke.spec.js` (generated docs / the live site), `smoke.spec.js`'s
force-update case (a newer Chromium refuses the test's
`Object.defineProperty` on `location.reload`), and two
`mobile.split-and-pwa.spec.js` reload cases that report "Failed to load WASM"
under WebKit.

### 7.6 One thing to know before deploying

`web/public/turmeric.{js,wasm}` are regenerated here, but `sw.js`'s
`CACHE_VERSION` is rewritten from `VERSION` at *build* time
(`injectSwVersion`, `apply: 'build'`), not on artifact regen. The precache is
cache-first, so shipping new wasm bytes under an unchanged version token would
serve a returning visitor the stale module. Nothing to do on this branch --
the repo already ties artifact regeneration to a release cut, which bumps
`VERSION` -- but do not deploy this bundle without one.
