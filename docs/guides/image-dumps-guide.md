---
title: Application Image Dumps
description: Warm-start a Turmeric program by saving and restoring a post-init continuation, Lisp/Smalltalk/pdumper style.
since: Phase AI
---

# Application Image Dumps

Expensive init -- loading config, parsing schemas, populating caches, building
indices, registering handlers -- is paid on every program start. For
long-running daemons it amortizes; for CLIs and short-lived workers it
dominates wall-clock latency.

Lisp (`save-lisp-and-die`), Smalltalk (image saves), and Emacs (`pdumper`)
solved this decades ago: dump the heap after init, load it back on later
starts, skip init. Turmeric's `tur/image` does the same with a **serializable
continuation** captured at a quiescent post-init point.

## What it does

`(with-image-cache-after-init PATH init loop)` runs `init` then `loop` on a
cold start, and -- crucially -- writes the continuation that runs `loop` to an
image file in between. On a later run, if a valid image exists, it restores
that continuation and jumps straight into `loop`, skipping `init` entirely.

The image file is a fixed 72-byte header (`src/runtime/image.h`) followed by
the Phase 21 TSER payload bytes of the captured continuation. No new wire
format -- it frames Phase 21's serial codec.

## When to use it

- Init is slow (>~100 ms) and deterministic.
- Cold startup latency matters (CLIs, test harnesses, short-lived workers).
- The post-init state can be expressed as a serializable continuation.

## When *not* to use it

- **The process holds live OS handles** across the capture point -- open
  sockets, GPU contexts, in-flight async I/O. These are `STAG_PTR` opaque and
  rejected by `TUR-E0018` at the `serial-shift` site; re-acquire them after
  load (see "Resources" below).
- **Init has side effects you must not skip** (e.g. it registers something the
  OS forgets between runs).
- **You ship a different binary every deploy** -- images are pinned to the
  binary that wrote them (see "Build-stamp safety").
- **The image could come from an untrusted source** -- loading an image is
  `eval` (see "Security").

## The APIs

```lisp
(load "stdlib/image.tur")

;; Primary combinator: split init from loop.
(with-image-cache-after-init "/var/cache/app.img"
  do-init      ; a (fn [] int) -- runs only on cold start
  main-loop)   ; a (fn [] int) -- runs on both cold and warm starts

;; Single-body wrapper (capture happens *after* body returns).
(with-image-cache "/var/cache/app.img" do-everything)

;; Low-level building blocks.
(save-image! "/var/cache/app.img")   ; capture here, write to disk
(load-image! "/var/cache/app.img")   ; restore + resume
```

`with-image-cache-after-init` is a **macro**, not a function: `init` and `loop`
are spliced into call position so the captured tail `(loop)` is a call to a
*named* top-level function. This is required -- the serial-shift collector
reconstructs frames by stable name, so the resumed tail must be a named
(serializable) continuation, never a heap closure. **Pass top-level `defn`
names**, not anonymous `fn`s, for `init` and `loop`.

`load-image!` returns `0` both on a validation failure *and* when the resumed
continuation legitimately evaluates to `0`. When the cold/warm decision matters,
gate on `image/loadable?` first (which `with-image-cache-after-init` does
internally) rather than on the return value.

## Globals: why your `def` isn't in the image

The image captures *the continuation*, which means **only values transitively
closed over by the captured frames** are in it. A top-level `def` that the
captured continuation doesn't reference is **not** in the image -- a foot-gun
if you expect whole-heap Lisp-image semantics.

> **Status:** A first-class `defimage-global` registry (plan AI3) that
> serialises declared mutable globals alongside the continuation, and a
> `TUR-W0706` lint for unregistered mutation reachable from a cache body, are
> **not yet implemented**. Until then, thread any post-init mutable state you
> need *through the captured continuation* (as a frame env), so it rides in the
> image. See `docs/upcoming/application-image-dumps-plan.md` AI3.

## Resources: the reload-hook pattern

OS-handle-backed values (files, sockets, GPU contexts) cannot be in the image.
The application must re-acquire them after load.

> **Status:** First-class `defimage-reload-hook` / `defimage-finalize-hook`
> forms and the standard-hooks library (plan AI5) are **not yet implemented**.
> For now, re-acquire resources explicitly at the top of your `loop` function
> (it runs on both cold and warm starts), keyed off whether the handle is
> already open. See `docs/upcoming/application-image-dumps-plan.md` AI5.

## Build-stamp safety

Loading an image written by a *different* binary is undefined behavior (frame
reconstruction maps names to this binary's functions). The build stamp catches
this cheaply at load time.

The stamp is a **SHA-256 of the running executable**, computed at load time
(via `/proc/self/exe` on Linux). Same binary -> same stamp; a different binary
-> a different file -> a different stamp -> the image is rejected cleanly (a
`0` return / cold-start fallback), never resumed as garbage.

> This differs from the plan's AI4.1 linker-section stamp (a two-pass build
> embedding the digest in a `.tur_build_stamp` section). Hashing the running
> executable delivers the same *safety* contract without platform-specific
> linker magic. It does **not** guarantee a byte-identical stamp across two
> independent builds of the same source (a reproducible-build nicety, not a
> safety property); two builds produce two binaries, so an image from build A
> will not load under build B -- which is exactly the safety behavior we want.

Inspect and validate images from the CLI without resuming them:

```sh
tur image-info   app.img            # print magic/version/stamp/payload-len/...
tur image-verify app.img            # structural check (magic/version/CRC)
tur image-verify app.img ./myapp    # also require the stamp to match ./myapp
```

`tur image-verify` exits `0` on success, `1` on a build-stamp mismatch, and
`2` on a structural failure (bad magic/version/CRC or unreadable file).

> **Dynamic libraries (AI4.4):** the stamp covers the main executable only. If
> the program `dlopen`s spice plugins or other `.so`s, the stamp does not see
> them; dynamic-spice + image-dumps is a follow-up. Treat images from a
> `dlopen`-ing binary with extra caution.

> **The `--unsafe-image-skip-build-check` escape hatch (AI4.3):** the stdlib
> path supports skipping the stamp check by passing `stamp = 0` to
> `image/load-resume-file!`. **Do not use this in production** -- it removes the
> only guard against resuming a foreign image. The matching `tur run --image`
> CLI flag (plan AI6.1/AI6.2) is not yet implemented.

## Security

**Loading an image is equivalent to `eval`** -- it resumes captured code. The
build-stamp check prevents *accidental* mismatch; it does **not** make a
malicious-but-stamp-matching image safe. Treat image files as trusted
executables:

- Store them under the same trust boundary as the binary (`/var/cache/app/`
  owned `root:root`; `~/.cache/app/` owned by the user).
- **Never** load an image from an untrusted source (HTTP, user upload, shared
  `/tmp`).

Signed images (an `Ed25519` signature in the header) are possible future work
if fleet warm-start becomes a use case.

## Prior art

| System | Mechanism | Cross-binary? |
|---|---|---|
| SBCL `save-lisp-and-die` | dump whole heap | pinned to the runtime |
| CCL `save-application` | dump whole heap | pinned |
| Smalltalk image save | dump object memory | pinned to the VM |
| Emacs `pdumper` | portable dump of post-init heap | pinned to the binary |
| Java CRaC | checkpoint/restore via CRIU | pinned |
| Linux CRIU | whole-process checkpoint | pinned |
| **Turmeric `tur/image`** | **serializable continuation + framed header** | **pinned via build stamp** |

Turmeric's twist: instead of dumping the whole heap, it captures a single
typed, `Serializable`-checked continuation at a quiescent point the application
chooses -- so the captured state is exactly what the typechecker allows to
cross the boundary.

## See also

- `docs/guides/serializable-continuations-guide.md` -- the Phase 21 substrate.
- `docs/upcoming/application-image-dumps-plan.md` -- the full plan (AI0--AI8),
  including the phases not yet implemented (AI3 globals, AI5 reload hooks,
  AI6.1 `tur run --image`, perf benchmarks).
- `src/runtime/image.h` -- the canonical header layout; `tests/image_unit.c`
  pins it.
