# turmeric-spices: pervasive `:int` stand-ins for handles, callbacks, and results

**Status:** Reported
**Severity:** Design defect / expressiveness hole, pervasive across the spice
ecosystem. Type checker provides ~zero protection on the public surface of
~half the spices. Highest-severity subset: 5 callback-typed-as-`:int` cases
where even a flagrantly wrong handler signature compiles silently.
**Discovered:** 2026-06-14, audit triggered by
`docs/reported/tourist-middleware-takes-req-not-ctx.md`.
**Scope:** `../turmeric-spices/spices/*/src/**/*.tur` (35 spices audited).

---

## Summary

Across `turmeric-spices`, opaque C handles (DB connections, sockets, GL
objects, audio devices, canvases), callback function pointers, `option<T>`
and `result<T,E>` values, response/request handles, and internal cons lists
are routinely declared as `:int` on public APIs. The recently-added CLAUDE.md
"No Lazy `:int` Stand-Ins -- STRICT RULE" exists precisely because of how
prevalent this turns out to be. Worst offenders (tourist, sqlite, postgres,
opengl, plutovg, osc, rtmidi) export entire APIs whose handle and callback
types convey nothing to the type checker.

This blocks the session-middleware plan (which is what surfaced the tourist
case), but the larger issue is that **new code written against these APIs
inherits the looseness** -- every wrapper, every spice that depends on
`tourist`/`sqlite`/`opengl` ends up parroting `:int` because that is what
the upstream signature says. Each downstream `:int` is a fresh place where
the compiler can no longer catch a swapped argument or a wrong-shape
callback.

Roughly 16 of 35 spices are clean (`ansi`, `c-dsl`, `ecs`, `ecs-raylib`,
`frame`, `glsl`, `json`, `linalg`, `math`, `plot`, `raygui`, `scscm`,
`sdf-raylib`, `signal`, `stats`, `test`, `tidal`, `watch`, `zlib`).
Most of those are pure Turmeric or compose cleaner sub-spices; they show
the right pattern is already standard practice where there isn't a C
handle in the way.

## Severity rubric used below

- **S1 -- Callback typed `:int`.** The type checker cannot enforce even a
  trivially-wrong handler shape. Highest priority.
- **S2 -- Public opaque handle typed `:int`.** Compiler treats DB
  connections, statements, canvases, windows, sockets, etc. as
  indistinguishable from each other and from machine integers.
- **S3 -- `result<T,E>` or `option<T>` returned as `:int`.** Forces every
  caller into inline-C or unsafe convention to extract the value. Lost
  ergonomics and lost exhaustiveness checking.
- **S4 -- Cons lists / linked lists typed `:int`.** Internal but spreads
  across every helper that walks them; small per-site cost, large total.

---

## S1 -- Callbacks typed `:int` (5 findings, fix first)

These are the most dangerous: handler signature is documented in prose only,
and an exported call site can pass a function of any shape with no
diagnostic.

| File:line | Function | `:int` actually is | Should be |
|---|---|---|---|
| `tourist/src/tourist/middleware.tur:58` | `use! [fn : int]` | `(fn [Request] option<Response>)` (per current tourist/app dispatch) -- but see the separate `tourist-middleware-takes-req-not-ctx.md` finding; the right shape is `(fn [Ctx] option<Response>)` | `(fn [Ctx] option<Response>)` |
| `tourist/src/tourist/dsl.tur:185,198,211,224,240` | `get!`, `post!`, `put!`, `delete!`, `any!` -- all take `handler : int` | `(fn [Ctx] Response)` route handler | `(fn [Ctx] Response)` |
| `httpd/src/httpd/server.tur:511,528` | `server-start [... handler : ptr<void>]`, `server-start-pool` | Raw C function pointer of shape `int64_t(*)(int64_t)` | `(fn [Request] Response)` (or named `Handler` newtype) |
| `rtmidi/src/rtmidi/in.tur:120` | `midi-in-set-callback [mi : int callback : int]` | `RtMidiCCallback` (`(fn [double bytes :ptr<u8> nbytes :int user :ptr<void>] :void)`) | Named callback type, ideally a `defopaque` over the function-pointer type |
| `osc/src/osc/server.tur:138` | `server-add-method [... handler : int]` | OSC liblo method handler | `(fn [path :cstr types :cstr argv :ptr<void> argc :int msg :Msg user :ptr<void>] :int)` or, since the docstring says "reserved for future FFI", drop the parameter until it has a real type |

**Why S1 first**: any further work on tourist routes, OSC servers, or MIDI
input compounds the surface area of these holes. Fixing them first lets
the downstream spices that depend on tourist (notebook? template? user
apps?) inherit real types as soon as they rebuild.

---

## S2 -- Opaque C handles typed `:int`

Grouped by spice. The fix in every case is one of:

- `defopaque Foo :ptr<void>` for an externally-allocated handle, OR
- `:ptr<__c_struct_name>` if the layout is owned by us and a real `defstruct`
  exists, OR
- `defopaque Foo :int` (last resort) if it really is an opaque integer key.

### tourist (11 findings)

| File:line | Function | What `:int` is | Should be |
|---|---|---|---|
| `tourist/dsl.tur:78` | `route-new` returns `:int` | `Route` handle | `defopaque Route` |
| `tourist/dsl.tur:107,122` | `route-pattern`, `route-handler` | `Route` handle | `Route` |
| `tourist/dsl.tur:144` | `route-call-handler` -> `:int` | `Response` handle | `Response` |
| `tourist/helpers.tur:28,43,58,77` | `text`, `html`, `json-body`, `redirect` -> `:int` | `Response` handle | `Response` |
| `tourist/param.tur:212,217` | `tourist-ctx-new`, `__tourist-ctx-alloc` -> `:int` | `Ctx` handle | `defopaque Ctx` (also needs an extension slot -- see the middleware finding) |
| `tourist/param.tur:111` | `tourist-ctx-caps [ctx : int] : int` | `ctx`->`Captures` | `[Ctx] : Captures` |
| `tourist/router.tur:45,143` | `router-compile`, `router-match` -> `:int` | `Pattern`, `Captures` | `defopaque Pattern`, `defopaque Captures` |
| `tourist/routing.tur:95` | `mount! [prefix : cstr item : int] : int` | `Route` or sub-app handle in; `Mount` out | parameterise on whatever the sub-app type is |
| `tourist/routing.tur:256,275` | `__dispatch-item`, `__url-map-dispatch` -> `:int` | `Response` handle | `Response` |

### httpd (4 findings beyond the S1 callback)

| File:line | Function | Should be |
|---|---|---|
| `httpd/request.tur:120` | `req-header [req : int name : cstr] : int` (return is `result<cstr>`) | `[Request cstr] : result<cstr>` |
| `httpd/response.tur:56,89,122,148,198,217,232,247` | Response constructors / accessors | `Response` opaque throughout |
| `httpd/server.tur:272` | `srv-call-handler [handler : ptr<void> req : int] : int` | Typed handler + `Request`/`Response` opaques |
| `httpd/write.tur:70,216,233` | `serialize-response`, `wbuf-bytes`, `wbuf-len` | `Wbuf` opaque, `Response` opaque |

### ~~sqlite (6 findings)~~ -- **fixed**

`defopaque Db` and `defopaque Stmt` are exported from `sqlite/db.tur` and
threaded through `db.tur`, `stmt.tur`, `row.tur`, and `error.tur`. The 6
original audit findings (`db-close`, `db-exec`, `db-prepare`, `db-query`,
`stmt-step`, `col-count`) all take the nominal type. Remaining S4 work
(`row : int` cons-list in `sqlite/row.tur`) is out of scope here.

### ~~postgres (5 findings)~~ -- **fixed**

`defopaque Conn` added in `postgres/db.tur` and threaded through `db.tur`,
`stmt.tur`, and `notify.tur`. All 5 audit findings (`db-close`, `db-exec`,
`db-query`, `stmt-prepare`, `notify-listen`) now take `conn : Conn`.
Adjacent `conn : int` parameters on `db-query-params`, `db-begin`,
`db-commit`, `db-rollback`, `stmt-exec-prepared`, `stmt-deallocate`,
`notify-unlisten`, and `notify-poll` were retyped in the same pass for
consistency. Internal `__pq-*-raw` helpers also take `Conn`. The
`PGresult*` rows handle (`rows : int` in `postgres/row.tur`) and the
`PGnotify*` notification handle (`n : int` in `postgres/notify.tur`)
remain as follow-up work.

### opengl (5 findings)

| File:line | Function | Handle is |
|---|---|---|
| `opengl/buffers.tur:23,37,70` | `make-vao`, `make-vbo`, `bind-vao` | `GLuint` VAO / VBO |
| `opengl/shaders.tur:27,74` | `compile-shader`, `shader-program` | `GLuint` shader / program |
| `opengl/window.tur:25` | `make-window` | `GLFWwindow*` (and a width/height pair -- those `:int`s ARE real ints) |

Proposed: `defopaque Vao`, `Vbo`, `Shader`, `Program`, `Window`. Note that
GL object names actually are `GLuint` (32-bit), but conflating window
pointers with shader IDs is still a category error.

### plutovg (5 findings)

| File:line | Function | Handle is |
|---|---|---|
| `plutovg/canvas.tur:79,102` | `canvas-create`, `canvas-destroy` | `plutovg_canvas_t*` |
| `plutovg/surface.tur:54` | `surface-create` | `plutovg_surface_t*` |
| `plutovg/path.tur:86` | `path-destroy` | `plutovg_path_t*` |
| `plutovg/font.tur:108` | `font-face-destroy` | `plutovg_font_face_t*` |

### osc (3 findings beyond the S1 handler param)

| File:line | Function | Handle is |
|---|---|---|
| `osc/msg.tur:31` | `msg-new` | `lo_message*` |
| `osc/bundle.tur:24,52` | `bundle-new`, `bundle-add-msg` | `lo_bundle*`, `lo_message*` |
| `osc/server.tur:110` | `server-free` | `lo_server_thread*` |

### raylib (3 findings)

| File:line | Function | Handle is |
|---|---|---|
| `raylib/audio.tur:49,66` | `load-sound`, `unload-sound` | `Sound*` (heap-copied) |
| `raylib/audio.tur:102` | `load-music-stream` | `Music*` (heap-copied) |

### ~~tls (2 findings)~~ -- **fixed**

`defopaque TlsCtx` exported from `tls/ctx.tur`; `defopaque TlsConn` exported
from `tls/conn.tur`. `tls-ctx-new` now returns `TlsCtx`; `tls-wrap-fd`
takes `ctx : TlsCtx` and returns `TlsConn`; every other ctx-* / conn-*
function parameter takes the nominal type. Because both constructors use
the literal `0` as a failure sentinel (no Result wrapper -- S3 follow-up),
`tls-ctx-null?` / `tls-conn-null?` predicates were added and exported so
callers no longer need a literal `(= ctx 0)` test that would no longer
typecheck. Both test fixtures (`ctx-lifecycle`, `tls-roundtrip`) updated
to import and use the new types and predicates. The `tls-wrap-fd` `fd`
parameter is still `:int` -- that one really is a file descriptor.

### rtmidi (2 handle findings)

`rtmidi/core.tur:131,149`, `rtmidi/in.tur:51`: `RtMidiPtr` (in/out) typed
`:int`.

### Smaller spices (one handle each)

- ~~`valkey/client.tur:50,101`, `valkey/cmd.tur:51` -- `redisContext*`~~
  **fixed in `tur-valkey` v0.2.0**: `defopaque Client` exported from
  `valkey/client.tur`; `client-close`, `client-ping`, all `cmd-*`, and all
  `pubsub-*` (subscribe/unsubscribe/publish/recv) parameters now take
  `c : Client`. `redisReply*` (`r : int` in `valkey/reply.tur`) and the
  `__msg*` push handle (`m : int` in `valkey/pubsub.tur`) remain as
  follow-up work.
- ~~`rtaudio/core.tur:82` -- `RtAudio` C++ instance~~
  **fixed in `tur-rtaudio` v0.2.0**: `defopaque Audio` exported from
  `rtaudio/core.tur`; `audio-free`, `audio-api`, and every `device-*` /
  `stream-*` function that took the backend handle now takes
  `a : Audio`. The `RtAudioDeviceInfo*` accessor handle (`di : int` in
  `rtaudio/devices.tur`) and the audio callback (`callback : int` in
  `rtaudio/stream.tur:62`, an S1-class hole) remain as follow-up work.
- ~~`regex/regex.tur:56,95` -- compiled regex~~ **fixed in `tur-regex` v0.2.0**:
  `defopaque Regex` + `defopaque Match`; all public parameters retyped.
  Result-typed returns also landed (`(Result Regex cstr)`, etc.) once the
  project-mode kind preload was wired up in 2026-06-14's
  `spice-defn-return-result-kind-mismatch` fix.
- ~~`png/reader.tur` -- image handle~~ **fixed**: `defopaque Img`
  exported from `png/info.tur`; `png-read`, `img-free`, all `img-*`
  accessors and `pixel-*` helpers now take `img : Img`. `png-write`
  retyped to match. Negative case errors with `TUR-E0001`.
- ~~`wav/reader.tur` -- `SNDFILE*`~~ **fixed**: `defopaque Wav` exported
  from `wav/info.tur`; `wav-open-read`, `wav-open-write`, `wav-close`,
  `wav-read-float`, `wav-seek`, and all `wav-*` accessors now take
  `w : Wav`. Negative case errors with `TUR-E0001`.

---

## S3 -- Optionals and Results collapsed to `:int`

Particularly painful in the tourist/httpd path because every handler that
reads a query param or header is forced to write inline-C to unwrap.

| File:line | Function | Should be |
|---|---|---|
| `tourist/param.tur:324,370` | `param`, `capture` -> `:int` (result<cstr>) | `result<cstr>` |
| `tourist/router.tur:244` | `captures-get` -> `:int` (result<cstr>) | `result<cstr>` |
| `httpd/request.tur:120` | `req-header` -> `:int` (result<cstr>) | `result<cstr>` |
| `tourist/app.tur:228`, `tourist/middleware.tur:114` | `mw-chain-run` -> `:int` (option<Response>) | `option<Response>` |

The middleware chain runner returning `option<Response>` is doubly
ironic given that the type *exists* in stdlib and the body literally
encodes "0 = none, non-zero = some". Same shape as `option<T>`; just
spell it.

---

## S4 -- Cons lists typed `:int`

Every cons-walker helper in `tourist/app.tur`, `tourist/middleware.tur`,
and `tourist/routing.tur` takes / returns `:int`. Examples:

- `tourist/app.tur:35,60,113,123` -- `__tourist-cons`, `__tourist-list-rev`,
  `__tourist-collect-mws`, `__tourist-collect-routes`
- `tourist/middleware.tur:25,32,114` -- `__mw-cons-head`, `__mw-cons-tail`,
  `mw-chain-run`
- `tourist/routing.tur:52,61,67` -- `__rt-cons`, `__rt-cons-head`,
  `__rt-cons-tail`

These are internal helpers, but they parrot the same shape as the
exported public API and obscure what the lists actually contain
(`list<Middleware>`, `list<Route>`, etc.). Once the public Route /
Middleware opaques exist, these should become `list<Route>` and
`list<Middleware>`.

---

## Per-spice verdict

| Spice | Verdict |
|---|---|
| ansi | clean |
| c-dsl | clean |
| ecs | clean |
| ecs-raylib | clean |
| frame | clean |
| glsl | clean |
| http | clean (audited as part of tourist sweep) |
| **httpd** | **S1 + S2 + S3 (Request/Response/Wbuf opaques + handler callback)** |
| json | clean |
| linalg | clean |
| math | clean |
| notebook | not audited (large, mostly string/parsing) |
| **opengl** | **S2: 5 handle types** |
| **osc** | **S1 + S2: 3 handles + 1 callback** |
| plot | clean |
| **plutovg** | **S2: 5 handle types** |
| ~~png~~ | ~~S2: 1 handle type~~ -- fixed (`defopaque Img`) |
| ~~postgres~~ | ~~S2: 5 handle types~~ -- fixed (`defopaque Conn`); `Rows`/`Notification` remain |
| raygui | clean |
| **raylib** | **S2: 3 handle types** |
| ~~regex~~ | ~~S2: 1 handle type~~ -- fixed in `tur-regex` v0.2.0 |
| ~~rtaudio~~ | ~~S2: 1 handle type~~ -- fixed (`defopaque Audio`); `DeviceInfo` handle + callback remain |
| **rtmidi** | **S1 + S2: 2 handles + 1 callback** |
| scscm | clean |
| sdf-raylib | clean |
| signal | clean |
| ~~sqlite~~ | ~~S2: 6 handle types~~ -- fixed (`defopaque Db`, `defopaque Stmt`); row alist S4 remains |
| stats | clean |
| test | clean |
| tidal | clean |
| ~~tls~~ | ~~S2: 2 handle types~~ -- fixed (`defopaque TlsCtx`, `defopaque TlsConn`) |
| **tourist** | **S1 + S2 + S3 + S4 (worst offender)** |
| ~~valkey~~ | ~~S2: 1 handle type (client)~~ -- fixed (`defopaque Client`); `redisReply*` and `__msg*` remain |
| watch | clean |
| ~~wav~~ | ~~S2: 1 handle type~~ -- fixed (`defopaque Wav`) |
| zlib | clean |

19 spices need work, 16 are clean. As of 2026-06-14: regex, sqlite, png, wav,
postgres, valkey, rtaudio, and tls are fixed (8 done); **11 remain**.

---

## Proposed fix directions

### Phase 1: Stop the bleeding (no spice changes)

- Land the CLAUDE.md "No Lazy `:int` Stand-Ins" rule (done 2026-06-14).
- Do not add new spice code or new turmeric/main code that types handles,
  callbacks, options, or results as `:int`. New code uses real types even
  if it has to introduce a `defopaque` in the calling spice as a local
  shim (and files a doc/reported note that the upstream still leaks `:int`).

### Phase 2: Fix S1 callbacks (tourist + httpd + rtmidi + osc)

These are isolated, high-value, and unlock the session plan. Order:

1. `tourist/dsl.tur` + `tourist/middleware.tur` + `tourist/app.tur` --
   introduce `defopaque Ctx`, `defopaque Request`, `defopaque Response`,
   `defopaque Route`, `defopaque Middleware`. Change `use!` and the
   `get!`/`post!`/`put!`/`delete!`/`any!` signatures to take typed
   callbacks. Coordinate with the separate
   `tourist-middleware-takes-req-not-ctx.md` finding (the middleware
   shape change and the type change should land together as
   `tur-tourist` v0.2.0).
2. `httpd/server.tur` -- type the `handler` parameter as a real function
   type instead of `:ptr<void>`.
3. `rtmidi/in.tur:120`, `osc/server.tur:138` -- type the callbacks.

### Phase 3: Fix S3 results / options on the request path

`tourist/param.tur`, `tourist/router.tur`, `httpd/request.tur`,
`tourist/middleware.tur:114`. Mechanical -- the bodies already build
the underlying `result<cstr>` / `option<Response>` shape; just declare
the right type. Big readability win at handler sites.

**Unblocked** (2026-06-14): `(defn f [] : (Result A B) ...)` at module
scope in a spice now typechecks and builds. See
[spice-defn-return-result-kind-mismatch.md](spice-defn-return-result-kind-mismatch.md)
for the fix. `tur-regex` v0.2.0 demonstrates the new shape end-to-end
(`regex-compile` returns `(Result Regex cstr)`, `regex-match` returns
`(Result Match cstr)`, etc.).

### Phase 4: Fix S2 opaque handles, spice by spice

In order of dependency / blast radius:

1. **sqlite, postgres, valkey** -- each spice in isolation; mostly
   single-file refactors. Land as v0.2.0 of each.
2. **tls** -- two opaques (`TlsCtx`, `TlsConn`), no downstream dependents
   in this audit.
3. **opengl, plutovg, raylib, raygui** -- graphics handles. Coordinate
   with `sdf-raylib` and `ecs-raylib` rebuilds.
4. **osc, rtmidi, rtaudio** -- audio/MIDI handles; coordinate with
   `tidal`/`signal` rebuilds.
5. **png, wav, regex** -- single handles; trivial.

### Phase 5: Fix S4 cons lists in tourist

Once `Route` / `Middleware` opaques exist, retype the cons helpers as
`list<Route>` / `list<Middleware>`. Internal but a good final cleanup.

## Validation

For each fixed spice:

- The spice's own tests pass with the new types.
- A new fixture demonstrates that *swapping* the arguments of a function
  (e.g. passing a `Stmt` where a `Db` is expected) is now a compile error
  on the new type, not a silent runtime crash.
- For S1 callbacks: a new fixture demonstrates that passing a callback
  of the wrong arity / shape is a compile error.
- For S3: a new fixture demonstrates exhaustive matching on the returned
  `result` / `option`.
- Downstream spices (anything depending on the fixed spice) recompile
  cleanly after adjusting their own signatures to consume the new types.

## Cross-references

- `docs/reported/tourist-middleware-takes-req-not-ctx.md` -- companion
  finding; the tourist S1+S2 fixes should land together with the
  middleware reshape (ctx-passing, post-response hook, ctx attrs slot).
- `docs/upcoming/v1/tourist-session-middleware-plan.md` -- blocked on
  both the middleware reshape and the tourist S1+S2 fix; plan SS4/SS5
  should be revised to target the new typed API once it lands.
- `CLAUDE.md` -- "No Lazy `:int` Stand-Ins -- STRICT RULE" (added
  2026-06-14) is the prospective gate that should prevent regrowth.
