# WASM Thread Support

**Status:** Not started. Deferred from `select-fair-blocking-plan.md` (Open
Question 4) and `thread-cancellation-plan.md` (Open Question 4).

**Prerequisites:**
- Phase SEL0--SEL2 (fair blocking `select`) complete.
- Phase TC0--TC2 (thread cancellation) complete.

**Last updated:** 2026-05-16

---

## Summary

The current WASM build (`just wasm`) compiles Turmeric with Emscripten using
`-sASYNCIFY` for cooperative async execution but does **not** enable pthreads.
As a result, any runtime code that calls `pthread_mutex_lock`,
`pthread_cond_wait`, or `tur_select_blocking` is either absent or silently
single-threaded in the browser.

This plan adds full pthread support to the Emscripten build so that the
threading primitives introduced in SEL0--SEL2 and TC0--TC2 work correctly in
the web REPL.

---

## Background: Emscripten Pthreads

Emscripten implements pthreads using Web Workers and `SharedArrayBuffer`. Each
pthread maps to a Worker thread; synchronisation uses `Atomics.*` operations on
shared memory.

### Browser Requirements

`SharedArrayBuffer` is only available in cross-origin isolated contexts. The
server must send two HTTP response headers on every page:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

These headers are currently absent from `web/worker.js` (the Cloudflare Worker
that serves the site).

### Build Requirements

| Flag | Purpose |
|---|---|
| `-pthread` | Enable pthreads in compiler and linker |
| `-sPTHREAD_POOL_SIZE=N` | Pre-spawn N worker threads at module load time |
| `-sASYNCIFY=1` | Keep (for top-level `await` support in the REPL) |
| `--shared-memory` | Required alongside `-pthread` for `SharedArrayBuffer` |

`-sASYNCIFY` and `-pthread` can coexist but require `-sASYNCIFY_IMPORTS` to
list any pthread-related asyncified functions. The safest path is to keep
ASYNCIFY as-is and add pthreads alongside it.

### Emscripten Pthread Caveats

- `pthread_cancel` is not implemented in Emscripten. The cooperative cancel
  design from `thread-cancellation-plan.md` (cancel flag + condvar) is
  unaffected by this -- it does not use `pthread_cancel`.
- `pthread_cond_timedwait` is available and works correctly under Emscripten
  with `-pthread`.
- The JS module grows from a single `.js` + `.wasm` file to `.js` + `.wasm` +
  `turmeric.worker.js`. All three files must be served from the same origin.

---

## Architecture

### Affected Files

```
src/CMakeLists.txt          -- add -pthread flags to tur_wasm emcc invocation
web/worker.js               -- add COOP/COEP headers to all responses
web/public/                 -- gains turmeric.worker.js alongside turmeric.js/.wasm
```

### CMakeLists.txt Changes

Add to the `emcc` invocation in `src/CMakeLists.txt`:

```cmake
-pthread
-sPTHREAD_POOL_SIZE=4
--shared-memory
-sEXPORTED_FUNCTIONS=...,_turi_wasm_init,...   # unchanged
```

Remove `-sASYNCIFY_STACK_SIZE` if it conflicts; otherwise keep it.

Also add `turmeric.worker.js` to the `POST_BUILD` copy step so the web bundle
picks it up.

### Cloudflare Worker Header Changes

`web/worker.js` must add COOP/COEP headers to every response:

```js
const headers = new Headers(response.headers);
headers.set('Cross-Origin-Opener-Policy', 'same-origin');
headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
return new Response(response.body, { ...response, headers });
```

This applies to all routes (the REPL page, the WASM files, and static assets).

---

## Phases

### Phase WT0 -- Build System: Enable Pthreads in Emscripten

**Goal:** Produce a WASM module that compiles and links with `-pthread` without
breaking the existing single-threaded eval path.

**Tasks:**
- [ ] Add `-pthread`, `-sPTHREAD_POOL_SIZE=4`, and `--shared-memory` to the
      `emcc` invocation in `src/CMakeLists.txt`
- [ ] Add `turmeric.worker.js` to the `POST_BUILD` copy step so it lands in
      `web/public/`
- [ ] Verify `just wasm` produces `turmeric.js`, `turmeric.wasm`, and
      `turmeric.worker.js` with no compile errors or warnings
- [ ] Verify the existing single-threaded REPL still evaluates basic expressions
      correctly (`just web-dev`, smoke-test in browser)

**Exit Criterion:** `just wasm` succeeds; basic eval works in browser.


### Phase WT1 -- Server: Cross-Origin Isolation Headers

**Goal:** Enable `SharedArrayBuffer` in the browser by serving COOP/COEP
headers from the Cloudflare Worker.

**Tasks:**
- [ ] Update `web/worker.js` to add `Cross-Origin-Opener-Policy: same-origin`
      and `Cross-Origin-Embedder-Policy: require-corp` to all responses
- [ ] Verify `SharedArrayBuffer` is defined in the browser console on the REPL
      page (`just web-dev`)
- [ ] Confirm that Monaco Editor (loaded via CDN) is served with a
      `Cross-Origin-Resource-Policy: cross-origin` header or is already
      same-origin; if not, switch to the self-hosted Monaco bundle in
      `web/node_modules/`
- [ ] Verify the doc panel, tour page, and roadmap page load without CORS
      errors after the header change
- [ ] Run `just smoke` (production smoke tests) against the dev server

**Exit Criterion:** No CORS errors; `SharedArrayBuffer !== undefined` in REPL.


### Phase WT2 -- Verify `tur_select_blocking` Under WASM

**Goal:** Confirm that the fair blocking `select` (SEL0--SEL2) compiles and
executes correctly inside the WASM module.

**Tasks:**
- [ ] Run `just wasm` with a build that includes the SEL1 `tur_select_blocking`
      implementation; confirm no emcc errors
- [ ] Write a small Turmeric snippet that exercises `select` over two channels
      with producer threads; paste it into the web REPL and verify it returns
      the correct `[index value]` result
- [ ] Verify no `Atomics.wait` deadlocks occur (Atomics.wait blocks the main
      thread in browsers -- confirm `tur_select_blocking` sleeps on a Worker
      thread, not the main thread)
- [ ] Check browser console for any pthread-related warnings

**Exit Criterion:** `select` over multiple channels works in the web REPL
without freezing the browser tab.


### Phase WT3 -- Verify Thread Cancellation Under WASM

**Goal:** Confirm that the cooperative cancellation primitives (TC0--TC2)
compile and function correctly in the WASM module.

**Tasks:**
- [ ] Confirm `tur_thread_cancel` and `tur_thread_cancelled` are exported in
      the `emcc` `-sEXPORTED_FUNCTIONS` list (or are reachable from Turmeric
      stdlib without direct JS calls)
- [ ] Note in `thread-cancellation-plan.md` that `pthread_cancel` is not
      available in Emscripten -- confirm the cooperative cancel flag design is
      unaffected
- [ ] Write a Turmeric snippet that starts a thread, cancels it, and joins it;
      paste into the web REPL and verify clean exit
- [ ] Check for memory leaks (waiter list cleanup) using browser DevTools
      memory profiler after running the cancel snippet

**Exit Criterion:** Cooperative cancel works in browser; no memory leaks.


### Phase WT4 -- Cleanup and Documentation

**Goal:** Close all deferred WASM items in the related plan documents.

**Tasks:**
- [ ] Update `select-fair-blocking-plan.md` Open Question 4: mark resolved
- [ ] Update `thread-cancellation-plan.md` Open Question 4: mark resolved
- [ ] Add a note to `docs/guides/threading-guide.md` describing WASM threading
      constraints (pool size, no `pthread_cancel`, COOP/COEP headers required
      for self-hosting)
- [ ] Update `CHANGELOG` / release notes

**Exit Criterion:** All deferred WASM items resolved; threading guide updated.

---

## Open Questions

1. **Atomics.wait on main thread:** **Resolved: Option A -- run eval in a
   dedicated Worker.**

   Browsers prohibit `Atomics.wait` on the main thread; Emscripten's
   `pthread_cond_wait` uses `Atomics.wait` internally. Running `turi_wasm_eval`
   synchronously from the main JS thread would throw whenever Turmeric code
   blocks on a channel or `select`.

   **Resolution:** Move all `turi_wasm_eval` calls off the main thread into a
   dedicated eval Worker. The main thread posts the input string to the Worker
   via `postMessage`; the Worker runs eval (which may block on pthreads
   primitives freely); the Worker posts the result back. The main thread
   remains unblocked throughout.

   ```
   main thread                eval Worker
       |                           |
       |-- postMessage(input) ---> |
       |                      turi_wasm_eval(input)
       |                      [may block on chan/select]
       |                      result ready
       | <-- postMessage(result) --|
       |                           |
   ```

   **Consequences:**
   - `-sASYNCIFY=1` is no longer needed for blocking eval; remove it from the
     `emcc` invocation in WT0 to simplify the build and avoid the known
     ASYNCIFY + pthreads interaction hazards.
   - The web REPL JS (`web/main.js` or equivalent) must be updated to
     communicate with the eval Worker rather than calling the WASM module
     directly.
   - The eval Worker file (`turmeric.eval-worker.js`) must be served from the
     same origin as the WASM files.

   **Tasks added to WT2:**
   - [ ] Create `web/eval-worker.js`: loads the WASM module, listens for
         `postMessage({ input })`, calls `turi_wasm_eval`, posts back
         `{ result }` or `{ error }`
   - [ ] Update `web/main.js` (or equivalent REPL entry point) to spawn the
         eval Worker and route input/output through `postMessage`
   - [ ] Remove `-sASYNCIFY=1` and `-sASYNCIFY_STACK_SIZE` from the `emcc`
         invocation in `src/CMakeLists.txt`
   - [ ] Verify that blocking `select` in the web REPL does not freeze the
         browser tab

2. **PTHREAD_POOL_SIZE:** **Resolved: Option A -- fixed pool of 4, documented.**

   The WASM build uses `-sPTHREAD_POOL_SIZE=4`. This pre-spawns 4 Worker
   threads at module load time. Programs that try to spawn more concurrent
   threads than the pool will block waiting for a thread to become available.

   **Documentation to add to `docs/guides/threading-guide.md`:**
   - The web REPL supports a maximum of 4 concurrent Turmeric threads.
   - Programs that spawn more than 4 threads will work correctly but thread
     creation beyond the pool limit incurs additional latency as Emscripten
     grows the pool on demand.
   - This limit does not apply to native (`tur run`) builds, which use
     unrestricted POSIX threads.

   **Future direction -- Option C (lazy pool growth):**
   Emscripten supports `PTHREAD_POOL_SIZE_STRICT=0`, which disables the hard
   pre-spawn limit and grows the Worker pool lazily on demand. This eliminates
   the cap at the cost of variable first-spawn latency on each new thread.
   Worth revisiting if user programs routinely exceed 4 concurrent threads in
   the REPL. No implementation work required now; note it in the threading
   guide as a known self-hosting tuning option.

3. **Monaco CDN vs. self-hosted:** **Resolved: already self-hosted, no action
   required.**

   Monaco is loaded from the local `monaco-editor` npm package via Vite's
   bundler (`import('monaco-editor')` and `new URL('monaco-editor/esm/...',
   import.meta.url)`). It is not fetched from any CDN. The COEP header will
   not block it. No changes needed in WT1.

4. **wasm-opt and pthreads:** **Resolved: Option A -- start at `-O2`, verify
   `-O3` separately.**

   Older `wasm-opt` (Binaryen) versions do not understand the threads memory
   model and can silently mis-optimize or strip shared memory annotations when
   invoked by `emcc -O3`. To avoid this risk during initial bring-up, the
   threaded WASM build starts at `-O2`.

   **Plan:**
   - WT0: change the `emcc` invocation in `src/CMakeLists.txt` from `-O3` to
     `-O2` when adding pthread flags.
   - WT2: once the full threading stack is verified correct at `-O2`, test
     `-O3` explicitly. If the output is identical and all fixtures pass, restore
     `-O3`. If not, keep `-O2` and document the reason in a comment in
     `src/CMakeLists.txt`.

   **Tasks added to WT0:**
   - [ ] Change `-O3` to `-O2` in the `emcc` invocation alongside the pthread
         flags

   **Tasks added to WT2:**
   - [ ] Re-run the threaded REPL smoke test with `-O3`; compare output against
         `-O2` build; restore `-O3` if clean or document why it is kept at
         `-O2`

---

## Related Work

| Reference | Notes |
|---|---|
| Emscripten pthreads docs | `emscripten.org/docs/porting/pthreads.html` |
| MDN SharedArrayBuffer | Explains COOP/COEP requirements |
| `select-fair-blocking-plan.md` | Source of deferred WASM Q4 |
| `thread-cancellation-plan.md` | Source of deferred WASM Q4 |

---

## Summary

**Recommendation:** Implement in phases WT0--WT4 after SEL0--SEL2 and TC0--TC2
are complete.

**Next step:** Begin WT0 by adding `-pthread` to the `emcc` invocation in
`src/CMakeLists.txt` and verifying `just wasm` still builds cleanly.
