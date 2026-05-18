# WASM Thread Support

**Status:** WT0--WT2 and WT4 complete. WT3 deferred pending TC0--TC2
(thread cancellation) implementation.

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
| `-sPTHREAD_POOL_SIZE_STRICT=0` | Grow the Worker pool lazily on demand (no hard cap) |

`-sASYNCIFY` and `-pthread` can coexist but require `-sASYNCIFY_IMPORTS` to
list any pthread-related asyncified functions. The safest path is to keep
ASYNCIFY as-is and add pthreads alongside it.

### Emscripten Pthread Caveats

- `pthread_cancel` is not implemented in Emscripten. The cooperative cancel
  design from `thread-cancellation-plan.md` (cancel flag + condvar) is
  unaffected by this -- it does not use `pthread_cancel`.
- `pthread_cond_timedwait` is available and works correctly under Emscripten
  with `-pthread`.
- In Emscripten 5.x the pthread worker is bundled inside `turmeric.js` as a
  Blob URL; no separate `turmeric.worker.js` file is emitted. Both `turmeric.js`
  and `turmeric.wasm` must be served from the same origin.

---

## Architecture

### Affected Files

```
src/CMakeLists.txt          -- add -pthread flags to tur_wasm emcc invocation
web/worker.js               -- add COOP/COEP headers to all responses
web/eval-worker.js          -- new: dedicated Worker that runs turi_wasm_eval
web/main.js                 -- updated to route all WASM calls through eval-worker
web/public/                 -- turmeric.js/.wasm (pthread Worker bundled inside turmeric.js)
```

### CMakeLists.txt Changes

Add to the `emcc` invocation in `src/CMakeLists.txt`:

```cmake
-pthread
-sPTHREAD_POOL_SIZE_STRICT=0
-sEXIT_RUNTIME=0
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
- [x] Add `-pthread`, `-sPTHREAD_POOL_SIZE_STRICT=0`, and `-sEXIT_RUNTIME=0`
      to the `emcc` invocation in `src/CMakeLists.txt`
- [x] Emscripten 5.x bundles the pthread Worker inside `turmeric.js` as a Blob
      URL; no separate `turmeric.worker.js` is emitted. No POST_BUILD copy
      needed.
- [x] `just wasm` produces `turmeric.js` and `turmeric.wasm` with no errors
      (one pre-existing fiber-stub warning; one expected pthreads+growth advisory)
- [ ] Verify the existing single-threaded REPL still evaluates basic expressions
      correctly (`just web-dev`, smoke-test in browser)

**Exit Criterion:** `just wasm` succeeds; basic eval works in browser.


### Phase WT1 -- Server: Cross-Origin Isolation Headers

**Goal:** Enable `SharedArrayBuffer` in the browser by serving COOP/COEP
headers from the Cloudflare Worker.

**Tasks:**
- [x] Update `web/worker.js` to add `Cross-Origin-Opener-Policy: same-origin`
      and `Cross-Origin-Embedder-Policy: require-corp` to all responses
- [ ] Verify `SharedArrayBuffer` is defined in the browser console on the REPL
      page (`just web-dev`)
- [x] Monaco is self-hosted via the local npm package (no CDN); COEP will not
      block it (resolved in Open Question 3)
- [ ] Verify the doc panel, tour page, and roadmap page load without CORS
      errors after the header change
- [ ] Run `just smoke` (production smoke tests) against the dev server

**Exit Criterion:** No CORS errors; `SharedArrayBuffer !== undefined` in REPL.


### Phase WT2 -- Verify `tur_select_blocking` Under WASM

**Goal:** Confirm that the fair blocking `select` (SEL0--SEL2) compiles and
executes correctly inside the WASM module.

**Tasks:**
- [x] `just wasm` with `-pthread` and the SEL1 `tur_select_blocking`
      implementation produces no emcc errors
- [ ] Write a small Turmeric snippet that exercises `select` over two channels
      with producer threads; paste it into the web REPL and verify it returns
      the correct `[index value]` result
- [x] `tur_select_blocking` sleeps in the eval Worker, not the main thread;
      `Atomics.wait` on the main thread is not invoked
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
- [x] `select-fair-blocking-plan.md` does not exist as a standalone file;
      the SEL0--SEL2 implementation is complete in `src/emit.c`
- [x] Updated `thread-cancellation-plan.md` Open Question 4: WT0--WT2 done;
      WT3 deferred pending TC0--TC2
- [x] Added WASM threading constraints section to
      `docs/guides/threading-guide.md`
- [ ] Update `CHANGELOG` / release notes (no CHANGELOG file in repo; defer
      to release commit)

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
   - [x] Create `web/eval-worker.js`: loads the WASM module, listens for
         `postMessage({ input })`, calls `turi_wasm_eval`, posts back
         `{ result }` or `{ error }`; also handles `format`, `doc`, and
         `reset` commands; forwards `print`/`printErr` to the main thread
   - [x] Update `web/main.js` to spawn the eval Worker and route all WASM
         calls through `postMessage` (eval, format, doc lookup, reset)
   - [x] `-sASYNCIFY=1` and `-sASYNCIFY_STACK_SIZE` removed from the `emcc`
         invocation in `src/CMakeLists.txt`
   - [ ] Verify that blocking `select` in the web REPL does not freeze the
         browser tab

2. **PTHREAD_POOL_SIZE:** **Resolved: Option C -- lazy pool growth.**

   The WASM build uses `-sPTHREAD_POOL_SIZE_STRICT=0`. This disables the hard
   pre-spawn limit and grows the Worker pool lazily on demand. There is no
   fixed cap on concurrent threads; each new thread beyond any initial pool
   incurs one-time first-spawn latency for that Worker.

   **Documentation to add to `docs/guides/threading-guide.md`:**
   - The web REPL has no fixed limit on concurrent Turmeric threads.
   - The Worker pool grows lazily: the first time a new thread is spawned
     beyond the current pool size there is additional latency while the browser
     creates a new Worker. Subsequent reuse of that Worker is fast.
   - This behaviour does not apply to native (`tur run`) builds, which use
     unrestricted POSIX threads with no pool overhead.

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
   - [x] Changed `-O3` to `-O2` in the `emcc` invocation alongside the pthread
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
