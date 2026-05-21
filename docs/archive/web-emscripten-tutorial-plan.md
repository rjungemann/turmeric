# Plan: Tutorial — Building Turmeric Projects for the Web with Emscripten

## Goal

Produce a guide at `docs/guides/web-emscripten-tutorial.md` that walks a
developer through compiling their own Turmeric project to WebAssembly using
Emscripten, hosting it in a browser page, and calling the WASM API from
JavaScript.

This is distinct from the existing `web/README.md` (which documents the
internal try-REPL build) and `docs/guides/c-integration-guide.md` (which covers
native C interop). The target audience is someone who wants to ship a Turmeric
program — or embed `libturi` — on a web page.

---

## Sections and Content Outline

### Front-matter / Header

```yaml
---
title: Building for the Web with Emscripten
category: Tutorials and Examples
description: Compile a Turmeric project to WebAssembly and run it in a browser
---
```

---

### 0. Prerequisites

What the reader needs before starting:

| Requirement | How to get it |
|---|---|
| Turmeric compiler (`tur`) built from source | `just build` |
| Emscripten SDK (`emsdk`) | https://emscripten.org/docs/getting_started |
| CMake 3.20+ | system package manager or cmake.org |
| Node.js 18+ (for the dev server) | https://nodejs.org |
| `just` (optional) | https://github.com/casey/just |

Verification commands:

```sh
tur --version
emcc --version
cmake --version
node --version
```

---

### 1. How the Turmeric WASM Build Works (Background)

Short conceptual section explaining the pipeline so readers understand *why*
the steps below are necessary:

1. `tur` compiles `.tur` source to C99.
2. Emscripten (`emcc`) compiles that C99 to `wasm32`, producing `.wasm` +
   a JS loader (`.js`).
3. The JS loader is bundled with HTML/JS via Vite (or served statically).
4. The browser instantiates the module and calls exported C functions from JS.

Key files inside the Turmeric repo that readers can study:

| File | Purpose |
|---|---|
| `src/web/wasm_glue.h` | Exported C API (`turi_wasm_init`, `turi_wasm_eval`, etc.) |
| `src/web/wasm_glue.c` | Implementation of the glue layer |
| `src/CMakeLists.txt` lines 273-370 | How the `tur_wasm` CMake target is defined |
| `web/main.js` | Reference JS integration using the WASM module |

---

### 2. Project A: "Hello WASM" — Inline Script, No Bundler

The simplest possible end-to-end example. The reader creates a single
`index.html` that loads the pre-built `turmeric.js` / `turmeric.wasm` from the
Turmeric repo's `web/public/` and evaluates a one-liner.

Steps:

1. Build the WASM module from the Turmeric repo:

   ```sh
   just wasm
   # Outputs: build-wasm/wasm/turmeric.js  build-wasm/wasm/turmeric.wasm
   ```

2. Create a minimal project directory:

   ```
   hello-wasm/
     index.html
     turmeric.js    ← copy from build-wasm/wasm/
     turmeric.wasm  ← copy from build-wasm/wasm/
   ```

3. Write `index.html` — show a minimal snippet using `TurmericModule`:

   ```html
   <script src="turmeric.js"></script>
   <script>
     TurmericModule().then(Module => {
       Module._turi_wasm_init();
       const input = '(+ 1 2)';
       const ptr   = Module.allocateUTF8(input);
       const resPtr = Module._turi_wasm_eval(ptr);
       console.log(Module.UTF8ToString(resPtr));
       Module._free(ptr);
     });
   </script>
   ```

   Explain `MODULARIZE=1`, `ALLOW_MEMORY_GROWTH`, and why `allocateUTF8` /
   `UTF8ToString` are needed.

4. Serve locally (no bundler required):

   ```sh
   npx serve hello-wasm
   # or: python3 -m http.server 8080
   ```

   Note the COOP/COEP headers required for `SharedArrayBuffer` (pthreads) and
   how to set them with a simple `_headers` / `server.js` file.

---

### 3. Project B: Evaluating a Full `.tur` File

Move from a string literal to loading a `.tur` source file at runtime.

Steps:

1. Write a sample Turmeric file `program.tur`:

   ```scheme
   (defn greet [name] :str
     (str-append "Hello, " name "!"))

   (println (greet "Web"))
   ```

2. Fetch and eval the file in JS:

   ```js
   const src = await fetch('program.tur').then(r => r.text());
   const ptr = Module.allocateUTF8(src);
   const res = Module._turi_wasm_eval(ptr);
   document.getElementById('output').textContent = Module.UTF8ToString(res);
   Module._free(ptr);
   ```

3. Add `program.tur` to the project directory and show the directory layout.

4. Explain `turi_wasm_eval_ex` for separated result/error output.

---

### 4. Project C: A Vite-Based Web App

Production-ready setup mirroring the existing `web/` directory.

Steps:

1. Scaffold the project:

   ```sh
   npm create vite@latest my-tur-app -- --template vanilla
   cd my-tur-app
   npm install
   ```

2. Copy `turmeric.js` and `turmeric.wasm` into `public/`.

3. Add COOP/COEP headers in `vite.config.js`:

   ```js
   export default {
     server: {
       headers: {
         'Cross-Origin-Opener-Policy': 'same-origin',
         'Cross-Origin-Embedder-Policy': 'require-corp',
       },
     },
   };
   ```

4. Create a `turmeric.js` wrapper module that initializes the singleton and
   re-exports `eval`, `evalEx`, `version`, and `reset`:

   ```js
   import createModule from '/turmeric.js';
   let _mod = null;
   export async function init() {
     if (_mod) return _mod;
     _mod = await createModule();
     _mod._turi_wasm_init();
     return _mod;
   }
   export async function evalCode(src) {
     const mod = await init();
     const ptr = mod.allocateUTF8(src);
     const rp  = mod._turi_wasm_eval(ptr);
     const out = mod.UTF8ToString(rp);
     mod._free(ptr);
     return out;
   }
   ```

5. Wire into `main.js` with a textarea + button + output div.

6. `npm run dev` — verify it works.

---

### 5. Rebuilding the WASM Module (Developer Workflow)

When the reader modifies Turmeric itself or the glue layer:

```sh
# From the Turmeric repo root
just wasm
# Then copy updated artifacts:
cp build-wasm/wasm/turmeric.js  my-tur-app/public/
cp build-wasm/wasm/turmeric.wasm my-tur-app/public/
```

Explain that `just wasm` runs `just docs` first so `stdlib/docstrings.tur` is
current before compilation.

Mention `just clean-wasm` to force a full rebuild.

---

### 6. Using the Doc-Lookup API

Show how to call `turi_doc_lookup` from JS to display inline documentation:

```js
function docLookup(name) {
  const mod = /* ... */;
  const ptr = mod.allocateUTF8(name);
  const rp  = mod._turi_doc_lookup(ptr);
  const doc = mod.UTF8ToString(rp);
  mod._free(ptr);
  return doc;
}
```

Tie back to `just docs` / `python3 tools/gendocs.py` so the reader understands
where the doc strings come from.

---

### 7. COOP/COEP and pthreads — Common Pitfalls

Dedicated troubleshooting section covering the most frequent WASM + browser
pain points:

| Problem | Cause | Fix |
|---|---|---|
| `SharedArrayBuffer is not defined` | Missing COOP/COEP headers | Add the two headers to every response |
| Module loads but `_turi_wasm_init` is undefined | Missing `-sEXPORTED_FUNCTIONS` flag at link time | Re-run `just wasm` with the correct export list |
| `TypeError: WebAssembly.instantiate` fails | Server sends `.wasm` as `text/plain` | Configure MIME type `application/wasm` |
| Hangs on first call after page load | pthread pool not initialised | Call `_turi_wasm_init` inside the `.then()` callback, not at top level |
| `allocateUTF8` is not a function | Wrong Emscripten version or missing `EXPORTED_RUNTIME_METHODS` | Ensure `stringToUTF8,UTF8ToString,lengthBytesUTF8` are exported |

---

### 8. Deploying to GitHub Pages

Walk through the existing `just deploy-web` workflow used by the Turmeric repo
itself, and how to adapt it for a standalone app:

1. Add a `deploy` script to `package.json` using `gh-pages`.
2. Set COOP/COEP headers via a `_headers` file (Netlify / Cloudflare Pages) or
   a Service Worker for GitHub Pages (which cannot set custom response headers).
3. Point `base` in `vite.config.js` to the sub-path if deploying to
   `username.github.io/my-tur-app/`.

---

### 9. Next Steps

Pointers to related guides:

- `docs/guides/c-integration-guide.md` — calling C from Turmeric
- `docs/guides/web-continuations-tutorial.md` — continuations in the web REPL
- `web/README.md` — the full try-REPL build
- `src/web/wasm_glue.h` — complete WASM API reference

---

## Implementation Notes

- All code snippets must be ASCII-only (no UTF-8 em dashes; use `--`).
- Keep inline C blocks minimal; this guide is JS-facing.
- Test snippets against the actual exported symbols listed in
  `src/CMakeLists.txt` lines 340-344 (`-sEXPORTED_FUNCTIONS`).
- The "Hello WASM" project (Section 2) should be reproducible in under 10
  minutes on a clean machine.

---

## File to Create

`docs/guides/web-emscripten-tutorial.md`

Estimated length: ~500-700 lines with all code snippets filled in.
