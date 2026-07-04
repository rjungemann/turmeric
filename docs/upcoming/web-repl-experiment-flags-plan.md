# Plan: opt into `--enable=<name>` experiments from the web REPL

**Status:** proposal (not started). **Area:** `src/web/wasm_glue.{c,h}`,
`web/main.js`, `web/public/eval-worker.js`.
**Goal:** let a Try Turmeric visitor opt in to one or more registered
experiments (currently `forall-kinds`, `forall-constraints`, `hkt-hrt`,
`forall-dict-pass`, `hrt-curried-result`, `vl-wide-functor`) via URL query
string, mirroring the CLI's `--enable=<name>` + `--allow-experimental`
pair, so demos and share-links that require an in-flight feature actually
compile in the browser instead of silently rejecting.

## Background

`turi_wasm_init` (`src/web/wasm_glue.c:80`) currently does:

```c
g_env = turi_env_new();
turi_env_set_diag_sink(...);
turi_init(false);
```

and returns. It never touches `g_allow_experimental` and never calls
`experiment_enable(...)`, so every `--enable=<name>` feature ships
default-off in the web REPL, and any experiment that also requires
`--allow-experimental` is unreachable from the browser at all.

Contrast with `tur check` (`src/main.c:1151`), which force-enables every
registered experiment for LSP diagnostics -- editors see the "maximally
permissive" compile. The web REPL sits at the opposite extreme: nothing is
on, and there is no wire to turn anything on.

The registry itself is already the right shape for this: `experiment_enable`
takes a name and a `XFSource`, so a browser opt-in is just a new source tag
and a caller.

## Goals

1. A visitor can hit `https://turmeric-lang.com/?enable=vl-wide-functor` (or
   `?enable=vl-wide-functor,forall-kinds`) and immediately eval code that
   uses those experiments, without a page rebuild.
2. Opt-in is **explicit per visit** -- a plain load of `/` still ships with
   zero experiments enabled, matching today's behavior. No experiment is
   silently on-by-default just because the browser is a "playground."
3. Unknown experiment names surface a first-class diagnostic in the REPL
   output, not a silent no-op.
4. Share-links produced by "Share" carry the enable set along with the code
   payload, so a shared demo of an experimental feature keeps working when
   the recipient opens it.
5. No regression to CLI/LSP/manifest paths -- this only adds a new `XFSource`
   and a browser-side caller; the registry, the warning machinery, and the
   `expires_at` gate stay identical.

## Non-goals

- Turning any experiment on by default in the web REPL. That is what the
  graduation path in `docs/guides/experimental-flags-guide.md` is for. A
  browser opt-in must not become a shadow "everyone gets it" ramp.
- A UI (checkbox panel, settings dialog) for toggling experiments. If we
  later want one, it lives on top of the query-string mechanism; the plan
  ships the wire first.
- Reworking `--allow-experimental` semantics. The browser adopts the same
  pair (`enable` + `allow`) that the CLI uses; if the CLI later collapses
  or renames them, the browser follows.
- Persisting the enable set to `localStorage` or cookies. Per-visit is the
  contract; a page reload without the query string resets to zero.

## Design

### New export: `turi_wasm_enable_experiment`

Add to `src/web/wasm_glue.{c,h}`:

```c
/* Enable the named experiment on the shared WASM environment.
 * Returns 0 on success, 1 if the name is not a registered experiment.
 * Must be called after turi_wasm_init() and before any eval that
 * needs the feature.  Mirrors --enable=<name> on the CLI. */
int turi_wasm_enable_experiment(const char *name);

/* Set the allow-experimental bit (mirrors --allow-experimental). */
void turi_wasm_allow_experimental(int allow);
```

Body of `turi_wasm_enable_experiment`:

```c
int turi_wasm_enable_experiment(const char *name) {
    if (!name || !*name) return 1;
    return experiment_enable(name, XF_SRC_WEB) ? 0 : 1;
}
```

Add a new `XFSource` value `XF_SRC_WEB` in `src/runtime/experiments.h`
alongside `XF_SRC_CLI` / `XF_SRC_MANIFEST`. The warning/lifecycle machinery
already routes by source; the new tag lets us tell "opted in from the URL"
apart from "wired in via CLI" in any future telemetry or diagnostic
tightening.

Export both symbols through the Emscripten `EXPORTED_FUNCTIONS` list in the
wasm build recipe (`Justfile` `wasm` recipe / `CMakeLists.txt` -- whichever
already lists `_turi_wasm_init`, `_turi_wasm_eval`, `_turi_wasm_reset`).

### Web-side: parse the query string, call the exports

In `web/public/eval-worker.js`, immediately after the successful
`_turi_wasm_init()` call (line 158), pull the enable set that main-thread
code passed into the worker and apply it:

```js
const initResult = turiModule._turi_wasm_init();
if (initResult !== 0) { /* existing error path */ }

if (self._pendingExperiments) {
    if (self._pendingAllowExperimental) {
        turiModule._turi_wasm_allow_experimental(1);
    }
    for (const name of self._pendingExperiments) {
        const cName = allocCString(turiModule, name);
        const ok = turiModule._turi_wasm_enable_experiment(cName);
        turiModule._turi_wasm_free(cName);
        if (ok !== 0) {
            self.postMessage({
                type: 'experiment-unknown',
                name,
            });
        }
    }
}
```

In `web/main.js`, extract the enable set from the URL before spawning the
worker and forward it:

```js
function readEnableSet() {
    const q = new URLSearchParams(window.location.search);
    const raw = q.get('enable');
    if (!raw) return { names: [], allow: false };
    const names = raw.split(',').map(s => s.trim()).filter(Boolean);
    // Convention: presence of ?enable= implies --allow-experimental,
    // since the CLI needs both to accept an in-flight feature. A visitor
    // typing a name has already opted in; no reason to require a second
    // parameter.
    return { names, allow: names.length > 0 };
}

const { names, allow } = readEnableSet();
worker.postMessage({ type: 'set-experiments', names, allow });
```

And in the worker's message handler, stash them for the init step:

```js
case 'set-experiments':
    self._pendingExperiments = msg.names;
    self._pendingAllowExperimental = !!msg.allow;
    break;
```

Order matters: `main.js` must send `set-experiments` **before**
`init` (or the worker must queue the init until it has seen the experiment
message). Simplest is to have `main.js` post both messages in order and
have the worker process them FIFO -- the init handler consults the stashed
fields when it runs.

### Diagnostic for unknown experiment names

When `_turi_wasm_enable_experiment` returns non-zero, the worker posts an
`experiment-unknown` message. `main.js` renders it into the REPL output
pane as a first-class message:

```
Unknown experiment: 'vl-wide-functorz'.
Registered experiments: forall-kinds, forall-constraints, hkt-hrt,
  forall-dict-pass, hrt-curried-result, vl-wide-functor.
```

The list of registered names is retrievable by adding a companion export
`turi_wasm_experiment_names(void)` that returns a comma-joined C string
(bump-allocated, freed with `turi_wasm_free`), so the message stays honest
as experiments come and go. Cheap: iterate `experiment_count()` /
`experiment_at(i)` and concatenate `->name`.

### Share-link integration

`web/main.js:753` already encodes the buffer into `#code=<base64>`. Extend
the encoder to also stash the current enable set:

```js
const parts = [`code=${encoded}`];
if (currentEnableSet.length > 0) parts.push(`enable=${currentEnableSet.join(',')}`);
window.location.hash = parts.join('&');
```

On load, `main.js:761` already reads `hash` via `URLSearchParams` -- add an
`enable` lookup there too, unioned with the `search` (query) value. Two
sources, one union, in that order of precedence:

1. `?enable=` in the URL search string (canonical, sharable in plain URLs).
2. `#enable=` in the URL hash (accompanies `#code=` in Share links).

Both feed the same `set-experiments` message. If both are present, union
the names.

## Milestones

| # | Deliverable | Files touched |
|---|---|---|
| WX-1 | Add `XF_SRC_WEB` to the `XFSource` enum; no callers yet. | `src/runtime/experiments.h`, `src/runtime/experiments.c` |
| WX-2 | Add `turi_wasm_enable_experiment` + `turi_wasm_allow_experimental` + `turi_wasm_experiment_names` exports; add to Emscripten export list; a unit fixture under `tests/wasm/` exercises them via a small C harness (mirrors the pattern of existing wasm-side glue tests). | `src/web/wasm_glue.{c,h}`, build recipe |
| WX-3 | Worker-side: accept `set-experiments`, apply after init, post `experiment-unknown` on failure. | `web/public/eval-worker.js` |
| WX-4 | Main-thread: parse `?enable=` from `location.search`, forward to worker, render `experiment-unknown` diagnostics into the REPL output pane. | `web/main.js` |
| WX-5 | Share-link: emit `enable=` alongside `code=` in the hash; parse both on load; union with `search`. | `web/main.js` |
| WX-6 | Docs: a short section in `docs/guides/experimental-flags-guide.md` describing the browser opt-in, with an example URL and the diagnostic wording. Link from the guide back to this plan (archived) once shipped. | `docs/guides/experimental-flags-guide.md` |

Milestones are independent enough to land in separate PRs. WX-1 + WX-2
must land together (the enum value is dead until the caller uses it);
WX-3 + WX-4 must land together (main-thread posts a message the worker
does not yet understand otherwise); WX-5 and WX-6 can trail.

## Testing

- **wasm-side unit** (WX-2): drive `turi_wasm_enable_experiment` from a
  small C harness, assert `experiment_is_enabled(name)` flips, assert an
  unknown name returns non-zero and does not flip anything.
- **Browser smoke** (WX-4): after building `just deploy-web` locally,
  open `http://localhost:<port>/?enable=vl-wide-functor` and eval a
  snippet that requires it. Confirm success; then load `/` (no query)
  and confirm the same snippet errors with the expected
  "requires --enable=vl-wide-functor" diagnostic.
- **Unknown name** (WX-3/WX-4): `?enable=nonsense` renders the
  registered-names list into the REPL output pane; the runtime still
  initializes cleanly and can eval non-experimental code.
- **Share round-trip** (WX-5): share a demo of an experimental feature,
  reload the produced URL in a fresh tab, confirm eval succeeds.

## Risks and open questions

1. **Do we want an allow-list?** Any registered experiment becomes
   web-opt-in-able by name. That is the same posture as the CLI, and the
   `expires_at` gate + `experiment_warn_if_used` warning still fire. If we
   ever ship an experiment that is *unsafe* to expose to arbitrary browser
   visitors (e.g. touches the sandbox boundary), we would want a
   descriptor-level `web_opt_in_allowed` bit. Not adding it now -- flag if
   any current or planned experiment needs it.
2. **Diagnostic warnings on every keystroke.** `experiment_warn_if_used`
   fires each time the elaboration path re-runs. The web REPL evals on
   Enter (not on keystroke), so the noise is bounded, but if we later add
   inline-as-you-type diagnostics we should silence experiment warnings
   the same way `tur_check_only` does. Note in the guide.
3. **Interaction with `turi_wasm_reset`.** `turi_wasm_reset` recreates
   `g_env` but does not touch the experiment registry (which is process-
   global). That is the desired behavior -- a `:reset` from the REPL
   should not silently drop the visitor's opt-ins -- but call it out so
   nobody "fixes" it later.
4. **`XF_SRC_WEB` vs. reusing `XF_SRC_CLI`.** Distinct tag costs nothing
   and preserves telemetry fidelity. Prefer distinct.

## Related

- `src/runtime/experiments.c` -- the registry and `EXPERIMENTS[]` rows.
- `src/main.c:5881` -- CLI `--enable=` / `--allow-experimental` parsing to
  mirror.
- `src/main.c:1151` -- LSP-side force-enable-all, the other existing
  non-CLI caller of `experiment_enable`.
- `docs/guides/experimental-flags-guide.md` -- user-facing docs for the
  flag mechanism.
