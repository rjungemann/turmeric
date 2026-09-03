# Application Image Dumps via Serializable Continuations -- Plan (AI0--AI8)

> **Status:** Core shipped. Phase 21 prerequisite is complete (PR #325;
> AI0 gate verified via `tests/fixtures/workflow-roundtrip`). Implemented:
> **AI1** (`src/runtime/image.{h,c}` + `tests/image_unit.c`), **AI2**
> (`stdlib/image.tur`: `save-image!` / `load-image!` / `with-image-cache` /
> `with-image-cache-after-init`), **AI4** (build stamp via SHA-256 of the
> running executable -- the safety contract of AI4.1 without the linker
> section), **AI6.3/AI6.4** (`tur image-info` / `tur image-verify`),
> **AI7.1** (`tests/fixtures/image-roundtrip`), **AI8.1-AI8.3**
> (`docs/guides/image-dumps-guide.md` + cross-links), **AI5**
> (reload/finalize hooks: `image/register-reload-hook!` /
> `image/register-finalize-hook!` + `defimage-reload-hook` /
> `defimage-finalize-hook` sugar, wired into `save-image!` / `load-image!` /
> both `with-image-cache` combinators; `stdlib/image_hooks.tur` standard
> hooks -- tracked-file table + stdio flush), **AI7.3**
> (`tests/fixtures/image-reload-hook`, `tests/fixtures/image-hooks-tracked`).
> **AI3** landed 2026-09-02 (`defimage-global` + `image/track-globals!`
> registry in `stdlib/image.tur`; the globals snapshot rides inside
> `payload_len` as a second section located by the header's `globals_offset`
> and is restored before resume; `TUR-W0706` lints the `init` root of
> `with-image-cache-after-init` via the G1 global-write walk; **AI7.2**
> `tests/fixtures/image-globals-roundtrip` +
> `tests/fixtures/warn-image-global-unregistered`). Two deltas from the sketch:
> tracking is a call at the top of `main` (same reason as the hooks), and the
> stdlib inline-C codec is the one that writes/reads the section -- the C
> `tur_image_write` still emits `globals_offset` 0.
> **Still open:** AI6.1/AI6.2 (`tur run --image` +
> `--unsafe-image-skip-build-check`), AI4.1 linker-section stamp,
> AI7.4/AI7.5 (error fixtures + perf benchmark). See the guide for the design deltas (build-stamp method;
> named-continuation constraint on the resumed tail; hooks register at the top
> of `main` rather than self-registering, since compiled top-level forms do not
> execute and warm starts skip `init`).
>
> Original plan text follows.

> **Original prerequisite note:** **Phase 21
> (`serial-shift` / `serial-reset` / `save-cont!` / `resume-cont!`) must
> ship first.** (Now complete.) This plan describes the
> *application-level* layer that turns the Phase 21 primitives into a
> Lisp/Smalltalk-style `save-image!` / `load-image!` workflow for warm
> startup.
>
> **Goal in one sentence.** Make `(tur run app.tur --image /var/cache/app.img)`
> on a second invocation skip expensive init by restoring a previously-saved
> post-init continuation, the same way Common Lisp `save-lisp-and-die` /
> Smalltalk image saves / Emacs portable-dumper work.
>
> **Last updated:** 2026-05-31
>
> **Related:**
> - [`call-cc-completion-plan.md`](call-cc-completion-plan.md) -- companion
>   work on the *delimited* continuation surface; uses the same `EX_RESET`
>   discipline this plan inherits.
> - `docs/guides/serializable-continuations-guide.md` -- the user-facing
>   semantics of `serial-reset` / `serial-shift` / `Serializable` typeclass.
> - `docs/archive/stubs-and-workarounds.md` §1.5 -- Phase 21 stub inventory
>   this plan depends on resolving.
> - `src/runtime/serial.{c,h}` -- TSER wire format, frame descriptors,
>   symbol registry (largely in place; missing the codegen + opaque-pointer
>   resolution).
> - Prior art: SBCL `save-lisp-and-die`, CCL `save-application`,
>   Smalltalk image save, Emacs `unexec` / `pdumper`, Java CRaC,
>   CRIU (Linux checkpoint/restore).

---

## Motivation

Expensive init -- loading config, parsing schemas, populating caches,
JIT/AOT-compiling user code, building search indices, registering effect
handlers and typeclass instances -- is paid on every program start. For
long-running daemons this is amortized; for CLIs, test harnesses, and
short-lived workers it dominates wall-clock latency and dwarfs the actual
work.

Lisp and Smalltalk solved this in the 1970s: dump the heap after init, load
it back on subsequent starts, skip init. Emacs ships pre-dumped (`pdumper`)
to cut startup from seconds to ~30 ms. Java CRaC and SBCL `save-lisp-and-die`
expose the same idea to applications.

Turmeric's runtime already has the substrate this needs: the `Serializable`
typeclass (`TUR-E0018`) statically rejects non-serializable captures at the
`serial-shift` site, frame descriptors live in the TSER format
(`src/runtime/serial.h`), and the symbol registry exists. **What's missing
is (1) the Phase 21 codegen that actually emits bytes, and (2) the
application-level scaffolding for the warm-start pattern.** This plan owns
(2) and is gated on (1).

### Goals

- Provide `stdlib/image.tur` with `save-image!` / `load-image!` /
  `with-image-cache` so applications can opt into warm-start in ~5 lines.
- Define a **build-stamp** discipline so an image saved by binary A is
  rejected (cleanly, not by segfault) when loaded by binary B.
- Define a **resource-reacquisition** hook so external state (open files,
  sockets, GPU contexts) is re-established on load instead of being silently
  garbage.
- Ship measurable speedup on at least one realistic workload (target: a
  schema-validating CLI that today spends >100 ms in init -- warm start
  under 10 ms).

### Non-goals

- Cross-binary or cross-version snapshot upgrade. Like SBCL, images are
  pinned to the binary that wrote them.
- Snapshotting *whole-process* state including non-Serializable globals.
  AI3 defines the globals story; values outside that scope are not in the
  image, period.
- Snapshotting fiber/scheduler state, async I/O in flight, or other live
  runtime resources. The image captures a single `serial-reset`'s
  continuation, taken at a *quiescent* point chosen by the application.
- A general-purpose CRIU-style process snapshotter. The image discipline is
  Turmeric-native (typed, Serializable-checked); use CRIU for the opaque
  cases.

---

## Architectural picture

```
   ┌─────────────────────── application ────────────────────────┐
   │                                                            │
   │  (defn main []                                             │
   │    (with-image-cache "/var/cache/app.img"                  │
   │      (fn []                                                │  ← warm-path here
   │        (expensive-init)                                    │
   │        (main-loop))))                                      │
   │                                                            │
   └────────────────────────────┬───────────────────────────────┘
                                │
                     ┌──────────▼──────────┐
                     │  stdlib/image.tur   │  ← AI2 (this plan)
                     │  save-image! /      │
                     │  load-image! /      │
                     │  with-image-cache   │
                     └──────────┬──────────┘
                                │
                     ┌──────────▼──────────┐
                     │  stdlib/workflow,   │  ← Phase 21 surface
                     │  stdlib/serial      │     (save-cont! / resume-cont!)
                     └──────────┬──────────┘
                                │
                     ┌──────────▼──────────┐
                     │  src/runtime/serial │  ← Phase 21 runtime
                     │  + serial-shift     │     (TSER, symbol registry,
                     │  codegen            │      reconstruct callbacks)
                     └─────────────────────┘
```

The image file is **the TSER bytes of a single captured `serial-cont`**,
prefixed by a fixed-layout `ImageHeader` (AI4). No new wire format; this
plan reuses Phase 21's TSER.

---

## Phase ordering at a glance

| Phase | Scope | Why this order |
|---|---|---|
| AI0 | Confirm Phase 21 readiness; pin SemVer surface | Hard dependency check before any work |
| AI1 | `ImageHeader` (magic / version / build-stamp / payload length) | Define the file format before writing serializers |
| AI2 | `stdlib/image.tur` -- `save-image!` / `load-image!` / `with-image-cache` | The user-facing API; thin shim over Phase 21 |
| AI3 | Globals discipline -- `defimage-global` registry | Names what *isn't* automatically captured |
| AI4 | Build-stamp embedding + check | Cross-binary safety; no segfaults on mismatch |
| AI5 | Resource-reacquisition hooks (`on-image-reload`) | External-state recovery contract |
| AI6 | CLI integration -- `tur run --image PATH` | Make the warm-start path discoverable |
| AI7 | Fixtures + perf measurement | Prove warm start works, prove the speedup |
| AI8 | Docs (`image-dumps-guide.md`) + audit closure | Make it findable; retire the §1.5 stub entry |

---

## Phase AI0 -- Confirm Phase 21 readiness

Application image dumps cannot ship before the primitives they sit on do.
This phase is a gate, not work.

- **AI0.1** Verify `save-cont!` returns non-NULL bytes for a non-trivial
  `serial-shift` (today it returns `NULL` --
  `stdlib/workflow.tur:33-37`). *Done when:* the Phase 21 plan's "codegen
  shipped" milestone is checked.
- **AI0.2** Verify `resume-cont!` round-trips a captured continuation
  (today it returns `0` -- `stdlib/workflow.tur:55-59`). *Done when:* a
  Phase 21 fixture demonstrates round-trip resume in the suite.
- **AI0.3** Verify the STAG_PTR placeholder in `src/runtime/serial.h` is
  either resolved (real pointer fixup) or explicitly out-of-scope (opaque
  pointers cannot cross the boundary, `TUR-E0018` rejects them). The latter
  is sufficient for this plan; the former unlocks more use cases.
- **AI0.4** Pin the SemVer surface of `stdlib/serial` and
  `stdlib/workflow` that this plan depends on, so a Phase 21 follow-up
  doesn't silently break `stdlib/image`. *Done when:* a one-line note in
  `serializable-continuations-guide.md` lists the API surface AI2 builds
  on.

---

## Phase AI1 -- `ImageHeader` file format

Every image file is `ImageHeader` followed by Phase 21's TSER bytes. Fixed
header, variable payload.

```c
/* src/runtime/image.h (new) */
#define TUR_IMAGE_MAGIC      0x54555249  /* "TURI" little-endian */
#define TUR_IMAGE_VERSION    1

typedef struct TurImageHeader {
    uint32_t magic;            /* TUR_IMAGE_MAGIC */
    uint32_t version;          /* TUR_IMAGE_VERSION (header format, not Turmeric) */
    uint8_t  build_stamp[32];  /* SHA-256 of the binary; see AI4 */
    uint64_t payload_len;      /* bytes of TSER data following the header */
    uint64_t created_unix_ns;  /* informational; not used for validation */
    uint32_t flags;            /* reserved, must be 0 */
    uint32_t header_crc32;     /* CRC32 of bytes [0, header_crc32) -- catches truncation */
} TurImageHeader;
```

- **AI1.1** Define `TurImageHeader` in `src/runtime/image.h` exactly as
  above. The header is 72 bytes, fixed layout, little-endian on disk
  regardless of host. *Done when:* `sizeof(TurImageHeader) == 72` is
  static-asserted in a unit test.
- **AI1.2** Implement `tur_image_write(FILE *f, const uint8_t *payload,
  size_t payload_len, const uint8_t build_stamp[32])` and
  `tur_image_read_header(FILE *f, TurImageHeader *out)`. *Done when:* a
  C-level round-trip test writes then re-reads the header faithfully and
  rejects bad magic / bad CRC / version mismatch with named errors.
- **AI1.3** Reserve error codes in the runtime error band for `IMAGE_BAD_MAGIC`,
  `IMAGE_BAD_VERSION`, `IMAGE_BAD_BUILD_STAMP`, `IMAGE_BAD_CRC`,
  `IMAGE_TRUNCATED`. These surface in `stdlib/image.tur` (AI2) as
  named `Result` errs.

---

## Phase AI2 -- `stdlib/image.tur` user-facing API

The whole user experience is three forms.

```lisp
;; Low level: explicit save/load.
(save-image! "/var/cache/app.img")            ; capture here, write to disk
(load-image! "/var/cache/app.img")            ; restore + jump

;; High level: idempotent warm-start wrapper.
(with-image-cache "/var/cache/app.img"
  (fn []
    (expensive-init)
    (main-loop)))
;; First run:  the fn body runs; an image is captured *after* expensive-init
;;             and *before* main-loop, written to the path, then main-loop runs.
;; Later runs: the image is loaded, control jumps straight into main-loop.
```

- **AI2.1** Implement `save-image!` in `stdlib/image.tur`:
  ```lisp
  (defn save-image! [path :cstr] :int
    (serial-reset
      (serial-shift k
        (let [bytes  (save-cont! k)
              stamp  (current-build-stamp)]
          (image-write-file! path bytes stamp)))))
  ```
  Returns 1 on success, 0 on failure. The image captures the
  continuation **at the `serial-shift` site** -- i.e. the rest of the
  caller of `save-image!`. *Done when:* a fixture saves an image and
  observes a non-empty file at the path with a valid header.
- **AI2.2** Implement `load-image!`:
  ```lisp
  (defn load-image! [path :cstr] :int
    (let [bytes (image-read-file! path)
          ok    (image-validate-stamp! bytes (current-build-stamp))]
      (if ok
        (resume-cont! bytes 0)
        (panic "image build-stamp mismatch"))))
  ```
  Returns the resumed continuation's value (control transfers; rarely
  returns to the caller). *Done when:* a fixture loads a previously
  saved image and observes the post-`save-image!` code path executing.
- **AI2.3** Implement `with-image-cache` as the ergonomic combinator:
  ```lisp
  (defn with-image-cache [path :cstr body :fn] :int
    (if (image-exists? path)
      (load-image! path)
      (do
        (body)               ;; run init + body
        (save-image! path)   ;; capture post-body state
        0)))
  ```
  *Caveat (document in AI8):* in this shape the image is captured **after**
  the body returns, so it's most useful when the body itself ends in a
  long-running `main-loop` and the user wants to dump *between* `main-loop`
  iterations. A second form, `with-image-cache-after-init`, takes
  `init` and `loop` as separate fns and dumps between them:
  ```lisp
  (with-image-cache-after-init "/var/cache/app.img"
    (fn [] (expensive-init))   ;; init -- runs only on cold start
    (fn [] (main-loop)))       ;; loop -- runs on both
  ```
- **AI2.4** All three APIs short-circuit to a clear error (`Result`-typed
  return; no segfault) when:
  - the path is unwritable / unreadable;
  - the file exists but the magic / version / CRC / build-stamp fails (AI4);
  - Phase 21 primitives return NULL (degraded mode).
  *Done when:* an `errors/image-bad-stamp` expect-error fixture passes.

---

## Phase AI3 -- Globals discipline (`defimage-global`)

The image captures *the continuation*, which means *only values transitively
closed over* end up in it. Top-level `def` bindings whose values mutate after
init are **not in the image** by default -- a foot-gun if the user expects
Lisp-image semantics.

Two complementary mechanisms:

- **AI3.1** Lint-only diagnostic `TUR-W0706`: any `(set! global ...)` reachable
  from the `body` argument of `with-image-cache` raises a warning unless the
  global is registered via `defimage-global`. *Done when:* an
  unregistered-mutation fixture warns; a registered one is silent.
- **AI3.2** New form `(defimage-global name :T initial)`: same as `def` plus
  registration in a `Serializable`-typed global table. On `save-image!`, the
  table is serialised alongside the continuation as a sibling TSER blob; on
  `load-image!`, the table is restored *before* the continuation resumes.
  *Done when:* a fixture mutates a `defimage-global` between cold and warm
  starts and the warm start observes the post-init value.
- **AI3.3** The global table is **part of the image**, not separate -- it
  rides inside `payload_len` as a second TSER section after the continuation
  bytes. AI1's header gains an optional `globals_offset` field for locating
  the second section. *Done when:* the round-trip C test handles
  zero-globals and many-globals payloads.
- **AI3.4** Document the rule: "if mutable global state matters after
  warm-start, declare it with `defimage-global`. Plain `def` is captured
  only if a captured closure happens to reference it."

---

## Phase AI4 -- Build-stamp embedding + validation

Loading an image written by a different binary is **undefined behavior**
(function pointers shift, layouts may differ). The build-stamp catches this
cheaply at load time.

- **AI4.1** At build time, the linker stamps a 32-byte SHA-256 of the final
  binary (excluding the stamp itself; computed via a two-pass build à la
  Linux `kallsyms`) into a `.tur_build_stamp` ELF/Mach-O section. The
  runtime exposes it as `tur_current_build_stamp(uint8_t out[32])`. *Done
  when:* two consecutive builds of the same source tree produce the same
  stamp; modifying any `.tur` source produces a different stamp.
- **AI4.2** `save-image!` (AI2.1) calls `tur_current_build_stamp` and writes
  the result into `ImageHeader.build_stamp`. `load-image!` (AI2.2) calls it
  again and compares; mismatch surfaces as `Result/Err(IMAGE_BAD_BUILD_STAMP)`.
  *Done when:* an image saved by one build and loaded by a different build
  fails cleanly with a named error, not a segfault.
- **AI4.3** Provide an escape hatch for development:
  `--unsafe-image-skip-build-check` on the CLI (AI6) bypasses AI4.2's
  validation. Strictly opt-in; the runtime prints a one-line warning at
  load time. *Done when:* a hot-iteration loop using this flag is
  documented in AI8 with a prominent "do not use in production" callout.
- **AI4.4** Static-link only: AI4 explicitly does not handle dynamic
  libraries' stamps. If the binary `dlopen`s anything (spice plugins,
  user-loaded `.so` files), the stamp covers only the main image. Document
  this in AI8; treat dynamic-spice + image-dumps as a follow-up.

---

## Phase AI5 -- Resource-reacquisition hooks (`on-image-reload`)

Files, sockets, GPU contexts, and other OS-handle-backed values cannot be
in the image (they're STAG_PTR opaque -- `TUR-E0018` rejects them at the
`serial-shift` site). The application must re-acquire them after load.

> **Status (shipped).** `stdlib/image.tur` provides
> `image/register-reload-hook!` / `image/register-finalize-hook!` (and
> `defimage-reload-hook` / `defimage-finalize-hook` sugar for the named-`defn`
> hook); the hook runners are wired into `save-image!`, `load-image!`, and both
> `with-image-cache` combinators (reload hooks on the warm path before resume;
> finalize hooks on the cold path before the bytes are written). The
> standard-hooks library `stdlib/image_hooks.tur` ships the tracked-file table
> (`image-hooks/track-file!` / `use-reopen-tracked!` / `slot-handle`) and a
> stdio flush (`use-flush-stdio!`). Fixtures: `tests/fixtures/image-reload-hook`
> and `tests/fixtures/image-hooks-tracked` (AI7.3).
>
> **Design delta from AI5.1's sketch:** a compiled program runs only `main`
> (top-level forms do not execute), so the `(defimage-reload-hook name ...)`
> form cannot self-register at load time. It instead *defines* a named hook,
> and the application *installs* it with `image/register-*-hook!` at the top of
> `main` -- a point that runs on both cold and warm starts. This is required:
> a warm start skips `init`, so a hook registered inside `init` would be absent
> on the very run that needs the reacquisition. The hook registry lives in a
> single function's static storage (no file-scope C global, which the
> single-file backend cannot link).

- **AI5.1** Add `(defimage-reload-hook name (fn [] ...))` -- registers a
  callback invoked after `resume-cont!` returns control but before the
  continuation resumes user code. Hooks run in registration order. *Done
  when:* a fixture registers a hook that opens a `FILE*`, dumps an image,
  loads it, and observes the reopened file pointer in the resumed code.
- **AI5.2** Hooks must themselves be Serializable-free (they're code
  pointers, not state). The hook *function* is referenced by stable
  symbol-key the same way Phase 21 reconstructs frames; no special
  serialisation needed. *Done when:* a hook fixture survives a save/load
  cycle.
- **AI5.3** Document the inverse: `(defimage-finalize-hook name (fn [] ...))`
  invoked just before `save-image!` writes bytes, for flushing buffers or
  releasing transient state. *Done when:* a fixture demonstrates a logger
  flushed before image save.
- **AI5.4** Provide a stdlib library of standard hooks for common cases:
  `stdlib/image_hooks.tur` with `reopen-stdio-streams`,
  `reopen-tracked-files`, `rebind-listening-sockets`. Each is opt-in via
  `(image-use-hook 'reopen-stdio-streams)`.

---

## Phase AI6 -- CLI integration

The library API (AI2) covers programmatic use. The CLI flag covers the
"point me at any program, dump on first run, restore on second" case.

- **AI6.1** Add `tur run --image PATH program.tur` to `src/main.c`'s argv
  loop. Semantics: equivalent to wrapping the program's entry point in
  `(with-image-cache-after-init PATH ...)`. *Done when:* `tur run --image
  /tmp/foo.img examples/hello.tur` produces `/tmp/foo.img` on the first
  invocation and the second invocation observes a measurable wall-clock
  speedup (target: 2x or better on `examples/cli-with-init.tur`).
- **AI6.2** Add `--unsafe-image-skip-build-check` (AI4.3). *Done when:*
  the flag is parsed and threaded through to `load-image!`.
- **AI6.3** Add `tur image-info PATH` -- prints the header (magic, version,
  build-stamp, creation time, payload size) without resuming. *Done when:*
  `tur image-info /var/cache/app.img` outputs a human-readable header.
- **AI6.4** Add `tur image-verify PATH` -- validates header, CRC, and
  build-stamp against the current binary; exits 0 on match, non-zero on
  any mismatch. Useful in CI/deploy scripts. *Done when:* the command
  succeeds on a fresh image and fails (with the right exit code) on a
  stale one.

---

## Phase AI7 -- Fixtures and perf measurement

Prove warm-start works end-to-end and prove the speedup.

- **AI7.1** Round-trip fixture: `tests/fixtures/image-roundtrip` -- a
  program that uses `with-image-cache-after-init` to skip a 50 ms
  `usleep` on warm start. Asserts cold-start wall-clock includes the
  sleep, warm-start wall-clock does not. (Wall-clock asserts use the
  existing `tests/fixtures/*/expected.timing` mechanism if present;
  otherwise add a minimal one.)
- **AI7.2** Globals fixture: `tests/fixtures/image-globals` -- mutates a
  `defimage-global` during cold start; warm start observes the mutated
  value. Asserts the warning fires for an unregistered global on cold
  start.
- **AI7.3** Reload-hook fixture: `tests/fixtures/image-reload-hook` --
  registers a hook that reopens a temp file by path; cold start writes
  to the file, warm start reads from it via the reopened handle.
- **AI7.4** Error fixtures:
  - `tests/fixtures/errors/image-bad-magic` -- truncated/corrupted file.
  - `tests/fixtures/errors/image-bad-build-stamp` -- valid file from a
    different binary (synthesised in the fixture's setup script).
  - `tests/fixtures/errors/image-bad-crc` -- header CRC bit-flipped.
- **AI7.5** Realistic-workload microbenchmark: a CLI that loads a 100 KB
  JSON schema, validates one document, and exits. Cold start vs. warm
  start. Target: warm < 10 ms, cold > 100 ms, ≥10× speedup. *Done when:*
  the benchmark lives under `benchmarks/image-warm-start/` and is
  documented in AI8 with reproduction steps.
- **AI7.6** All fixtures pass `bash tests/run.sh` and snapshot per
  CLAUDE.md's fixture rule.

---

## Phase AI8 -- Docs

Make the feature findable and the foot-guns visible.

- **AI8.1** New guide: `docs/guides/image-dumps-guide.md`. Sections:
  - "What it does" (Lisp/Smalltalk image dump analogy).
  - "When to use it" (slow init, fast cold startup, deterministic
    post-init state).
  - "When not to use it" (process holds live network sessions; init
    has side effects you don't want to skip; you ship a different
    binary every deploy).
  - "The three APIs" (`save-image!`, `load-image!`, `with-image-cache`).
  - "Globals: why your `def` isn't in the image, and `defimage-global`
    when it should be."
  - "Resources: the reload-hook pattern, and the standard hooks library."
  - "Build-stamp safety, the `--unsafe-image-skip-build-check` foot-gun,
    and the dynamic-spice caveat."
  - "Security: deserialising an image is `eval`. Treat image files as
    *trusted code*."
  - "Prior art" comparison table (SBCL, CCL, Smalltalk, Emacs pdumper,
    Java CRaC, CRIU).
- **AI8.2** Extend `serializable-continuations-guide.md`'s "Comparison to
  Alternatives" table with a row for **Application Image Dumps** linking
  to AI8.1.
- **AI8.3** Update `docs/archive/stubs-and-workarounds.md` §1.5: mark
  Phase 21 closure (assuming AI0 passed) and add a sibling note pointing
  at this plan as the application-layer continuation of that work.
- **AI8.4** Update `docs/guides/compiler-flags-guide.md` with the new
  `--image PATH` / `--unsafe-image-skip-build-check` flags.
- **AI8.5** Cross-link from `call-cc-completion-plan.md` Appendix A's
  `tur_cloneable_cont_clone` note (it shows up there as the "warm
  restart" cousin -- in-process clone vs. cross-process image).

---

## Security note (called out for emphasis)

**Loading an image file is equivalent to executing arbitrary code**, the
same way `eval`ing a string is. The build-stamp check (AI4) prevents
*accidental* mismatch; it does **not** prevent a malicious image that
matches the build-stamp from doing whatever the program is allowed to do.

Treat image files as trusted executables:
- Store them in directories with the same trust boundary as the binary
  itself (`/var/cache/app/` owned root:root; `~/.cache/app/` owned by the
  user).
- Never load an image from an untrusted source (HTTP, user upload, shared
  tmp).
- Future work (out of scope here): signed images. An `Ed25519` signature in
  `ImageHeader` would let CI sign images for a fleet to load. Track this as
  a follow-up if/when fleet warm-start becomes a use case.

AI8.1's "Security" section calls this out as the first thing under
"When not to use it."

---

## Exit criteria for "shipped"

- `tur run --image /tmp/foo.img examples/cli-with-init.tur` measurably
  speeds up second-and-later invocations (AI6.1 / AI7.5 target met).
- All AI7 fixtures green under `bash tests/run.sh`; snapshots regenerated.
- `image-dumps-guide.md` published and linked from the user-facing TOC.
- `stubs-and-workarounds.md` §1.5 marked resolved; build-stamp validation
  closes the cross-binary footgun.
- `tur image-info` / `tur image-verify` work against real images and are
  documented.

---

## Open questions

- **AIQ1.** Should `with-image-cache-after-init` be the *primary* API
  (AI2.3's recommended shape), with the bare `with-image-cache` form
  demoted to "for advanced users"? *Tentative:* yes -- the
  init-vs-loop split is what users actually want; the single-fn form is a
  trap unless the body ends in a long-running loop.
- **AIQ2.** Should AI3's globals table be a single TSER blob, or one
  serialised entry per `defimage-global`? Single blob is simpler;
  per-entry survives partial-corruption recovery and enables future
  partial-load optimization. *Tentative:* single blob for AI3, revisit if
  partial recovery becomes a real need.
- **AIQ3.** Should `tur run --image` *default* to a path
  (`~/.cache/turmeric/$(basename program).img`) when the flag is given
  with no argument, matching Emacs `pdumper`'s ergonomics? *Tentative:*
  no -- require an explicit path so users opt into the cache location.
  The standard cache path goes in the guide as the recommended choice.
- **AIQ4.** Does the existing fiber-context machinery
  (`src/async/fiber_ctx_*.S`) play well with `serial-shift`'s frame
  serialisation, or do they fight? Phase 21's plan owns this question;
  AI0 inherits the answer.
