# Emscripten Spice Support + Web-App Scaffold Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-23
> **Type:** Tooling / Web Platform

---

## Overview

Today, `just wasm` builds a single artifact -- the `try.turmeric-lang.com` REPL
(`src/web/wasm_glue.c`). The compiler can already invoke `emcmake` for cmake
dependencies when `tur build --target wasm` is passed (see
`src/compiler/pkg.c:1375`), but that path is not exercised by any spice and
there is no end-user workflow for shipping a Turmeric *application* (as opposed
to the REPL) to the browser.

This plan lays out:

1. A **per-spice compatibility matrix** for Emscripten, so users know which
   `turmeric-spices` work in the browser today, which need work, and which
   never will.
2. A **v0 web-app scaffold** -- one command that produces a Vite project with a
   single page, a WASM module containing the user's Turmeric program, and (when
   needed) a `<canvas>` wired to a graphical/audio spice such as Raylib or
   OpenGL.
3. The **`:web` manifest key** in `build.tur` that lets spice authors declare
   whether they need a canvas / audio context / file system, and the build
   tooling that consumes it.

The goal is that a user with a `build.tur` and no web knowledge can run
`tur new-web my-game` or `tur build --target web` and get a deployable static
site that runs their Turmeric program in the browser.

---

## Goals / Non-Goals

### Goals (v0)

- One opinionated path: **Vite + a single `index.html` + single `<canvas>`**.
- `tur build --target web` produces `web-dist/` (static, deployable to GH
  Pages, Netlify, Cloudflare Pages).
- Raylib programs run unmodified: `init-window`, the main render loop, and
  `swap-buffers` work, integrated with `emscripten_set_main_loop`.
- OpenGL programs run unmodified through the GLFW Emscripten shim
  (`-sUSE_GLFW=3` + WebGL2).
- Pure-Turmeric and inline-C-only spices "just work" with no per-spice
  changes.
- `tur add` of an unsupported spice (e.g. `tur-postgres`) emits a clear error
  at configure time, not a confusing link error.

### Non-Goals (v0)

- Hot-reload of `.tur` source in the browser (the existing REPL does that;
  apps recompile from CLI).
- React/Vue/Svelte integration; the scaffold is plain HTML + a single
  `<canvas>`/`<pre>` mount point.
- Multi-page apps, routing, SSR.
- Bundling shaders, fonts, images out of the box -- v0 ships static-asset
  passthrough only.
- Worker-based off-main-thread execution (the REPL uses pthreads; apps will
  start single-threaded on the main loop).
- WebGPU / WebRTC / WebTransport bindings.

---

## Per-Spice Emscripten Compatibility Matrix

| Spice | Tier | Browser status | Notes |
|-------|------|----------------|-------|
| `tur-test`     | 1 pure | ✅ works | No I/O. |
| `tur-math`     | 1 pure | ✅ works | No I/O. |
| `tur-c-dsl`    | 1 pure | ✅ works | Compile-time codegen; runtime is a no-op. |
| `tur-glsl`     | 1 pure | ✅ works | Same as above; output is shader text. |
| `tur-scscm`    | 1 inline-C | ⚠️ partial | String compiler works. OSC client needs WebSocket bridge (out of scope for v0). |
| `tur-tidal`    | 1 inline-C | ✅ works | Pure string transformation. |
| `tur-opengl`   | 2 cmake | ✅ via shim | GLFW Emscripten port; restrict to OpenGL ES 3.0 subset (WebGL2). |
| `tur-raylib`   | 2 cmake | ✅ first-class | Raylib 5.5 has `PLATFORM_WEB`; main loop must be driven by Emscripten. |
| `tur-sqlite`   | 2 cmake | ✅ via MEMFS/IDBFS | Use MEMFS by default; IDBFS opt-in via `:web :persist true`. |
| `tur-png`      | 3 cmake | ✅ works | libpng + zlib both compile clean under emcc. |
| `tur-plutovg`  | 3 cmake | ✅ works | No platform deps; canvas needed only to display the surface. |
| `tur-json`     | 3 cmake | ✅ works | yyjson is portable C. |
| `tur-regex`    | 3 cmake | ✅ works | PCRE2 has Emscripten support upstream. |
| `tur-http`     | 3 cmake | ⚠️ remap | mbedTLS won't reach the network; remap to `fetch()` via `EM_JS`. v0: stub it with a clear error. |
| `tur-wav`      | 3 cmake | ✅ works | Pure decode/encode; no audio output by itself. |
| `tur-osc`      | 3 cmake | ❌ | UDP not available in browsers; would need WebSocket-OSC bridge. |
| `tur-rtaudio`  | 3 cmake | ❌→🔬 | Replace backend with Web Audio (`AudioWorklet`); large effort, defer past v0. |
| `tur-rtmidi`   | 3 cmake | ❌→🔬 | Replace backend with Web MIDI; smaller than rtaudio but defer. |
| `tur-postgres` | 3 cmake | ❌ | libpq has no browser story. Hard-error in `tur build --target web`. |
| `tur-valkey`   | 3 cmake | ❌ | TCP client -- same as above. |

Legend: ✅ works as-is or with the listed shim. ⚠️ runtime errors / partial.
❌ never works in browser. 🔬 has a research-grade path that's out of v0.

---

## The `:web` Key in `build.tur`

Spice authors and app authors describe their browser needs declaratively in
`build.tur`. The build tool reads this when `--target web` is passed and
configures the scaffold accordingly.

```turmeric
(defpackage my-game
  :name    "my-game"
  :version "0.1.0"
  :spices  #{
    "raylib" #{:url    "https://github.com/rjungemann/turmeric-spices"
               :ref    "raylib-v0.1.0"
               :subdir "spices/raylib"}
  }
  :web #{
    :canvas        true              ; mount a <canvas id="canvas">
    :canvas-size   [800 600]
    :audio         true              ; wire up Emscripten Web Audio output
    :persist       false             ; IDBFS for tur-sqlite, opt-in
    :main-loop     :raylib           ; :raylib | :emscripten | :none
    :title         "My Game"
    :lazy-assets   ["assets/music/long-track.ogg"]  ; opt-in fetch-on-demand
    ;; :threads true is the default; opt out only for static-hosting reasons
  })
```

### Spice-side declaration

Library spices add a `:web` block to advertise *what they require* of the
host page. The app's `:web` block wins on conflict; spice `:web` blocks are
unioned. Example for `tur-raylib`:

```turmeric
(defpackage tur-raylib
  ...
  :web #{
    :requires  #{:canvas :audio}
    :main-loop :raylib           ; raylib's WEB build owns the frame loop
    :cmake-options #{:PLATFORM "Web"
                     :SUPPORT_MODULE_RAUDIO "ON"}
  })
```

`tur-opengl` would declare `:requires #fx{:canvas}` and add
`-sUSE_GLFW=3 -sFULL_ES3=1 -sMIN_WEBGL_VERSION=2`. `tur-sqlite` would declare
no canvas/audio but add `-sFORCE_FILESYSTEM=1`.

### How the build tool consumes it

`tur build --target web` walks the dependency graph, collects every spice's
`:web` block, unions the requirements, and:

1. Writes/refreshes `web-dist/index.html` from a template, inserting the
   canvas, title, and audio-unlock affordances.
2. Composes the final `emcc` link command from the union of
   `:cmake-options` and built-in flags (`-sMODULARIZE=1`,
   `-sALLOW_MEMORY_GROWTH=1`, etc.).
3. Errors loudly if any spice in the graph is on the ❌ list above (with a
   pointer to the relevant section of this doc).
4. Picks a main-loop strategy (see next section).

---

## Main-Loop Strategy

Browsers can't block a while-true loop -- the page would freeze. Three
strategies, selected by the union of `:main-loop` declarations:

| Strategy | Trigger | What we do |
|----------|---------|------------|
| `:none` | No spice requests a loop | Run `main()` once; user is expected to register callbacks (e.g. an `onclick`). Used for offline tools (`tur-plutovg` rendering to PNG, scscm compiling text). |
| `:emscripten` | `tur-opengl`, custom requestAnimationFrame games | Caller writes the loop body as a Turmeric function; the scaffold calls `emscripten_set_main_loop_arg` once and exits `main()`. Requires a small helper in `tur-opengl` or a new `tur-web` spice. |
| `:raylib` | `tur-raylib` present | Raylib's `PLATFORM_WEB` build *already* expects to be driven by `emscripten_set_main_loop`. The user's `while (!window-should-close?)` body becomes the frame callback. Needs a build-time rewrite or a runtime shim that yields one iteration per call. |

For v0, `:raylib` does what Raylib's own examples do: ship a thin
`raylib/web` module (Turmeric-side) exposing
`(run-main-loop frame-fn)`; users opt into it when targeting web. Code that
keeps the native `while` form gets a clear migration error.

---

## v0 Web-App Scaffold

`tur new-web my-app` (new subcommand) writes:

```
my-app/
  build.tur                 -- with :web block already populated
  src/main.tur
  web/
    index.html              -- single page, <canvas>, audio-unlock button
    main.js                 -- imports TurmericModule, calls turi_wasm_eval on main.tur
    style.css
    vite.config.js
    package.json            -- vite + nothing else
  .gitignore
```

Running `tur build --target web` from `my-app/`:

1. Compiles user `.tur` sources to bytecode embedded in the WASM module.
2. Builds all cmake-dep spices through `emcmake` (already supported).
3. Links a `my-app.wasm` + `my-app.js` ES module into `web/public/`.
4. Runs `vite build` (invoked by the tool, not the user) to produce
   `web-dist/`.

`tur dev --target web` runs `vite dev` with a watcher that recompiles the
WASM on `.tur` changes.

### Choice of scaffold

Vite is the right v0 default because:

- Already used by `web/` for the REPL -- no new toolchain in the repo.
- Native ES-module dev server matches Emscripten's `-sMODULARIZE=1` output.
- Zero-config for static `.wasm`/`.data` asset handling.
- Trivial to swap out later (the scaffold is a template, not a runtime).

---

## Phases

### Phase W0 -- Compatibility audit + this doc

- Land this plan.
- Add a `:web {:supported true|false :reason "..."}` stub to every spice's
  `build.tur` so the build tool has machine-readable signal. Default
  `:supported false` for the ❌ row above.

### Phase W1 -- `tur build --target web` for pure spices

- Wire `:web` parsing into `pkg.c`.
- Produce a minimal `index.html` + `main.js` + `app.wasm` for
  pure-Turmeric / inline-C-only programs (no canvas).
- Build with `-pthread` by default (matches REPL); template a `_headers`
  file with COOP/COEP for Cloudflare/Netlify, document the GH Pages
  caveat.
- Wire `*args*` from `?arg=foo&arg=bar` plus a JS `turi_wasm_set_args`
  escape hatch callable from `main.js` before `turi_wasm_eval`.
- Preload `web/assets/**` into MEMFS via Emscripten `--preload-file`;
  honor `:web :lazy-assets [...]` as the per-file fetch-on-demand
  opt-out.
- Hard-error on any spice with `:supported false`, printing the offending
  spice(s) and a pointer to the compatibility matrix in this doc.
- Validate end-to-end with `tur-tidal` + `tur-scscm` "compile a tune to
  text" demo.

### Phase W2 -- Canvas + OpenGL

- Add `tur-opengl` `:web` block with GLFW shim flags.
- Ship a `(opengl/web/run-main-loop frame-fn)` helper.
- Port one existing `tests/fixtures/opengl/*` triangle demo to the new
  scaffold; document.

### Phase W2.5 -- Raylib audio spike

- Build the minimal raylib + raudio web demo end-to-end *before*
  committing to W3's scope. Upstream issue
  [raysan5/raylib#690](https://github.com/raysan5/raylib/issues/690) and
  the live `audio_music_stream` example on raylib.com suggest this works
  out of the box; the spike confirms it against our toolchain and
  `:web :audio true` plumbing.
- Document the AudioContext user-gesture requirement (browsers won't
  start audio until a click/keypress). v0 mirrors what Raylib's own
  examples do and pushes the unlock affordance to the user; no elaborate
  scaffolding.

### Phase W3 -- Raylib

- Add `tur-raylib` `:web` block (`PLATFORM=Web`, `USE_GLFW=3`,
  `SUPPORT_MODULE_RAUDIO=ON`).
- Ship `(raylib/web/run-main-loop frame-fn)`.
- Update `tur-raylib` docs with the platform divergence (no blocking
  while-loop on web) and a one-screen example that runs both natively and
  in the browser.

### Phase W4 -- `tur new-web` scaffold generator

- Implement the subcommand. Template lives at
  `templates/web-app/`. `--with raylib` / `--with opengl` flags choose the
  starter `main.tur`.

### Phase W5 -- Storage + audio polish

- `tur-sqlite` `:web :persist true` -> IDBFS preload + `FS.syncfs` on quit.
- Audio-unlock button (browsers require a user gesture before
  `AudioContext.resume()`); abstract into a tiny `audio-unlock` helper in
  the scaffold.

### Phase W6 (post-v0) -- Network + audio I/O

- `tur-http` rewritten to call `fetch()` via `EM_JS`.
- Decide rtaudio/rtmidi: AudioWorklet path vs. a separate `tur-webaudio`
  spice that doesn't pretend to be rtaudio.
- OSC over WebSocket bridge for `tur-scscm` / `tur-osc`.

---

## Resolved Decisions

The following were originally open questions; resolved 2026-05-23.

1. **Bundling.** Single monolithic `app.wasm` for v0. Reserve a
   `:web :side-module true` opt-in for spices, so per-spice lazy-loaded
   WASM modules can be enabled later without breaking apps.
2. **Threads.** Pthreads on by default, matching the REPL. Scaffold ships
   a `_headers` template for COOP/COEP (`Cross-Origin-Opener-Policy:
   same-origin`, `Cross-Origin-Embedder-Policy: require-corp`); GH Pages
   gets a documented caveat. Users can opt out via `:web :threads false`
   for naive static hosts.
3. **File-system surface.** MEMFS by default, lost on tab close. Spices
   that want persistence (mainly `tur-sqlite`) opt in via `:web :persist
   true`, which preloads from IDBFS at boot and calls `FS.syncfs` on
   quit. No NODEFS in browser builds.
4. **`*args*`.** Parsed from `?arg=foo&arg=bar` in `location.search` by
   default; ordering follows query-string order. A
   `turi_wasm_set_args(args[])` JS escape hatch lets `main.js` override
   before `turi_wasm_eval`. CLAUDE.md's rule about how Turmeric code
   reads args (`*args*` / `stdlib/args.tur` only) is unchanged.
5. **Asset pipeline.** `web/assets/**` is preloaded into MEMFS at boot
   via Emscripten `--preload-file`, addressable from Turmeric by the same
   relative path the native target would use. A per-file
   `:web :lazy-assets ["assets/music/long-track.ogg" ...]` opt-in
   switches selected files to fetch-on-demand for cases where a big blob
   shouldn't block startup.
6. **`tur-raylib` audio.** Assumed working per upstream issue
   [#690](https://github.com/raysan5/raylib/issues/690); confirmed by a
   W2.5 spike before W3 implementation begins. The AudioContext
   user-gesture requirement is documented as the user's responsibility;
   no scaffolded unlock UI in v0.
7. **Error UX.** `tur build --target web` hard-errors when the dep graph
   contains a spice with `:web :supported false`, naming the spice(s)
   and pointing at the compatibility matrix in this doc. No auto-stubs,
   no alternative suggestions in the error text itself (those live in
   the matrix).

---

## Relationship to existing work

- The WASM REPL (`web/`, `src/web/wasm_glue.c`) stays exactly as-is; nothing
  in this plan touches it. The new app scaffold is a sibling target, not a
  replacement.
- `src/compiler/pkg.c:1375` already branches on `target == "wasm"` for
  `emcmake`. v0 reuses that branch and adds the `:web` manifest parsing
  alongside.
- `docs/archive/history/plutovg-spice-plan.md` and
  `docs/archive/scscm-tidal-spices-plan.md` are consistent with this -- both
  spices fall on the ✅ side of the matrix and would be the first non-trivial
  demos of `tur build --target web`.
