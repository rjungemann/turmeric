# Decommissioning the `#lang` layer axis

Status: plan (surface audited against the tree at d76822e0b; not implemented).
Introduced: 2026-09-08
Supersedes: Part 1 of `docs/upcoming/reader-envelopes-and-stringed-graduation-plan.md`
(that doc keeps Part 2, the envelope axis / literate `.tur.md`, and Part 3,
`turmeric/at-exp`; both are unaffected by this plan and neither is a layer).

## Overview

Two changes that must land together, in this order:

1. **`#s"..."` becomes an always-on reader macro.** No `#lang` token, no
   `#use-reader-macros`. The only thing a program needs in order to use owned
   Strings is `(load "stdlib/string.tur")`.
2. **The layer axis is deleted.** `LANG_LAYERS[]`, `LangLayerSet`,
   `SourceFile.lang_layers`, `TuriEnv.lang_layers`, `detect_lang_layered`'s
   layer out-params, `lang_layers_apply_readers` /
   `lang_layers_apply_semantic`, `GRADUATED_LAYERS[]`, the
   `g_manifest_experiments_scoped` global, `tur lang-layers`, the wasm
   `layers` registry array, and the Try Turmeric layer checkboxes all go away.

After this, the `#lang` grammar is:

```
#lang <base>[/<dialect>]
```

and nothing else on the line. A trailing token is a hard error (`TUR-E0330`,
reworded), with a one-release migration window in which the single token that
was ever legal -- `stringed` -- is accepted as a warned no-op.

### Why the axis goes, not just its one member

The layer axis was built to hold a *set* of additive syntax features. In its
whole life it has held two rows, never more than one at a time:

- `refined` (semantic) -- graduated 2026-08-01 in v0.33.0, aged out of
  `GRADUATED_LAYERS[]` at 0.38.0. Its lesson was that a semantic layer is not
  a second enable path; it *is* its `EXPERIMENTS[]` row, scoped to one file.
  That makes `--enable=` the real mechanism and the layer a per-file alias
  for it.
- `stringed` (reader) -- one `#`-dispatch, `#s"` => `(string/from-cstr $body)`
  (`lang_layers.c:39-57`). Every other `#`-dispatch in the language
  (`#map{}`, `#set{}`, `#rat{}`, `#refine{}`, `#r{}`, `#json()`, `#fx{}`,
  `#reads`, `#writes`, `#rx""`) is unconditional. `stringed` is the outlier,
  not the pattern.

The two features that would have justified keeping the axis both turned out to
belong somewhere else:

- **at-exp** takes over `@` and changes how `{`/`}` read inside a body. It
  changes how the base reads a byte, so it is a *base dialect*
  (`#lang turmeric/at-exp`), not an additive dispatch.
- **Literate `.tur.md`** decides which bytes the base reader sees at all. A
  `LAYER_READER` hook is handed a `ReaderMacroRegistry` and can do exactly one
  thing -- register a `#`-dispatch (`lang_layers.h:38`). It never sees the
  source text. Literate is an *envelope*, a source-to-source pass.

So the axis has zero prospective members and one actual member that should
never have been optional. Keeping a registry, a bitset threaded through four
compile paths, a CLI subcommand, a wasm export, and a UI control for that is
carrying a generalization no one uses. CLAUDE.md already says to graduate a
layer to always-on rather than let layers accumulate; this is the same
instruction applied to the mechanism itself.

If a future feature genuinely needs per-file, order-independent, additive
configuration, that bridge gets crossed then -- with a design informed by
whatever the feature actually is, rather than by a table shaped for `#s"`.

### What is NOT changing

- **Base dialects.** `#lang turmeric`, `turmeric/curly-infix`,
  `turmeric/neoteric`, `turmeric/sweet`, and the legacy `sweet-exp` alias all
  stay exactly as they are. `lang_base_from_name` (`reader.c:4532`) is
  untouched. The Try Turmeric dialect picker keeps its radio group.
- **`#use-reader-macros`.** The user-facing per-file reader-macro mechanism
  stays. It is the supported home for a one-off syntax convenience, and it is
  what the deleted layer axis was competing with.
- **Experiments.** `--enable=<name>`, `EXPERIMENTS[]`, `GRADUATED[]`, the
  manifest `:experiments` key, and TUR-W0060/W0061/W0063 are untouched. Only
  the layer-to-experiment *bridge* dies, because it had no users after
  `refined` graduated.
- **Autoloading `stdlib/string.tur`.** Explicitly out of scope. Putting String
  on the autoload list (`stdlib_autoload.c`) would drop the `(load)` too, but
  it pulls a refcounted-heap type into every compilation unit and changes the
  ABI surface of every emitted TU. Separate decision, separate plan.

---

## The surface being removed

Audited against the tree. Every anchor below is a real call site.

### Compiler core

| File | What |
|---|---|
| `src/compiler/lang_layers.c` | whole file (registry, hooks, `GRADUATED_LAYERS[]`, apply fns) |
| `src/compiler/lang_layers.h` | whole file |
| `src/CMakeLists.txt:193` | `compiler/lang_layers.c` in the source list |
| `src/compiler/diag.h:396-400` | `LangLayerSet` typedef |
| `src/compiler/diag.h:437-442` | `SourceFile.lang_layers` field + comment |
| `src/compiler/diag.h:462-479` | `detect_lang` / `detect_lang_layered` declarations |
| `src/compiler/reader.c:4407-4420` | `lang_layers_apply_readers` + `lang_layers_apply_semantic` calls at reader init |
| `src/compiler/reader.c:4551-4650` | `detect_lang_layered` layer-token classification loop |
| `src/compiler/reader.c:4656-4662` | `detect_lang` wrapper |
| `src/compiler/elab_toplevel.c:939-975` | the `(load)`-path lang sweep; `sfile->lang_layers` |
| `src/compiler/pkg.c:680-692` | `detect_lang` strip before manifest parse |
| `src/runtime/globals.c:288-290`, `globals.h:405-409` | `g_manifest_experiments_scoped` -- **dead once the semantic bridge goes**; its only reader is `lang_layers.c:173` |
| `src/runtime/experiments.c:188, 381` | comments that cross-reference `GRADUATED_LAYERS[]` |

### CLI

| File | What |
|---|---|
| `src/main.c:171-212` | `detect_and_adjust_lang` -- layer out-param, TUR-E0330 emit |
| `src/main.c:921-948, 1140-1163, 1247-1270` | three compile entry points threading `lang_layers` into `SourceFile` |
| `src/main.c:2999-3005` | manifest `:experiments` -> `g_manifest_experiments_scoped` |
| `src/main.c:7210` | `tur fmt`'s `detect_lang` (signature churn only) |
| `src/main.c:7715-7735` | eval-blob `#lang` detection -> `env->lang_layers` |
| `src/main.c:9072` | subcommand name table (`"lang-layers"`) |
| `src/main.c:9136` | `tur --help` line |
| `src/main.c:10522-10578` | `cmd_lang_layers` |
| `src/main.c:12134-12135` | dispatch |
| `src/cli/completion.c:73, 132, 250, 284` | zsh/bash completion for `lang-layers` |

### Interpreter / REPL

| File | What |
|---|---|
| `src/turi/env.h:366` | `TuriEnv.lang_layers` |
| `src/turi/env.h:578-583`, `env.c:605-622` | `turi_env_apply_lang(env, rt, layers)` |
| `src/turi/eval.c:11972-12012` | inline `#lang` union-into-session (`env->lang_layers |= layers`) |
| `src/turi/eval.c:12080` | `sfile->lang_layers = env->lang_layers` |
| `src/turi/repl.c:1688-1712` | `#lang` meta-line handling, "unknown #lang layer" message |

### Try Turmeric / wasm

| File | What |
|---|---|
| `src/web/wasm_glue.c:390-443` | `turi_wasm_set_lang` -- accepts a full directive tail with layers |
| `src/web/wasm_glue.c:485-546` | `turi_wasm_lang_registry` -- the `"layers":[...]` array |
| `src/web/wasm_glue.h:144` | registry doc comment |
| `src/web/wasm_lsp.c:133-160` | `detect_lang_layered` + `file.lang_layers` |
| `web/main.js:1194-1218` | `parseLangDirective` returns `layers` |
| `web/main.js:1236-1247` | `LANG_REGISTRY_FALLBACK.layers` |
| `web/main.js:1386-1430` | `renderLangMenu` layer-checkbox rendering |
| `web/main.js:1432-1462` | `onLangControlChange` / `reconcileLangPicker` layer handling |
| `web/main.js` (`setLangDirective`, `currentLangSelection`) | `{base, layers}` shape |
| `web/try/index.html:114, 122-126` | the "Layers" column, `#lang-layers` div, button `title` |
| `web/styles.css:1827-1860` | `.lang-row-disabled`, `.lang-row-empty`, `.lang-chip`, the two-column `.lang-menu-cols` |
| `web/tests/lang-picker.spec.js:120-142` | "layer checkboxes toggle the trailing token in place" |
| `web/README.md:151` | documents the `set_lang` tail |

### Tests

| Path | What |
|---|---|
| `tests/wasm_glue_lang_unit.c` | asserts `"name":"stringed"` in the registry and the layer on/off round-trip -- **fails loudly** |
| `tests/run-flags.sh:947-995` | `lang-layer-toggle-off`, `lang-layer-same-set-no-reset` REPL assertions |
| `tests/fixtures/lang-layer-stringed/` | `#lang turmeric stringed` + `#s"..."` |
| `tests/fixtures/lang-layer-stringed-sweet/` | same under `turmeric/sweet` -- the only pin of `#s"` under a non-default base |
| `tests/fixtures/lang-trailing-tokens/` | trailing tokens stripped, not leaked into the body |
| `tests/fixtures/string-reader-macro/` | `#use-reader-macros "stdlib/string-reader.tur"` -- **becomes a hard error** |
| `tests/fixtures/errors/lang-layer-unknown/` | TUR-E0330 message text |
| `tests/fixtures/errors/lang-layer-retired-name/` | TUR-E0330 for `refined` |
| `tests/fixtures/refine-graduated-lang-layer-noop/` | stale: only `actual.*`/`turi.*` artifacts, no `input.tur` -- delete the directory |

### Docs

`docs/guides/syntax-guide.md` (lines 19-21, 370, 448, and all of Part 2.5 at
497-580), `docs/guides/reader-forms-guide.md:262-281`,
`docs/guides/strings-guide.md:123-152`, `CLAUDE.md` ("`#lang` Layers --
curated only", ~40 lines), `CHANGELOG.md`, plus superseded-banners on
`docs/archive/lang-layers-plan.md` and
`docs/archive/try-turmeric-lang-toggle-plan.md`.

---

## Phases

### D0 -- Bookkeeping (no code)

1. Trim Part 1 out of
   `docs/upcoming/reader-envelopes-and-stringed-graduation-plan.md` and point
   it here. That doc keeps the envelope axis and at-exp, both of which stand
   on their own; leaving a second, now-stale description of the `stringed`
   graduation in the tree is exactly the drift this repo's archiving rule
   exists to prevent.
2. Confirm no external consumer names a layer. Verified at plan time:
   `../turmeric-spices` (827da51) has zero hits for `stringed`,
   `lang-layers`, or `string-reader`; `turmeric-playground` has none; nothing
   under `examples/`, `tutorials/`, `benchmarks/`, `validation/`, `tvm/`,
   `man/`, `editors/`, `emacs/`, `vim-syntax/`, `vscode-syntax-ext/`,
   `tools/`, `scripts/`, or `.github/` references either. Re-run before
   landing:
   ```sh
   grep -rn "stringed\|lang-layers\|string-reader" \
     --exclude-dir=.git --exclude-dir=node_modules --exclude-dir=dist .
   ```
3. Confirm nothing else defines a `#s"` reader macro:
   ```sh
   grep -rn "reader-macros/define 's" stdlib tests examples tutorials
   ```
   Expected: `stdlib/string-reader.tur` only.

### D1 -- `#s"` becomes always-on

Add `reader_macros_install_builtins(ReaderMacroRegistry *reg, Arena *arena,
SymbolTable *st)` to `src/compiler/reader_macros.c`, with the body of
`stringed_reader_hook` (`lang_layers.c:39-57`) moved into it verbatim. Call it
from `read_all_with_registry_from` (`reader.c:4407`), in the slot the
`lang_layers_apply_readers` call occupies today -- after `reg` is established,
before the read loop.

**Keep the `reader_macros_lookup(reg, "s", '"')` early return.** It is
load-bearing for two separate reasons, and it is not obvious from the call
site:

- The interpreter/REPL registry is env-lifetime (`env.h:364`) and sees this
  function on every eval.
- The batch-compile registry is **strict** and is *shared across `(load)`*:
  `load_expand_forms` passes `e->user_macros` back into
  `read_all_with_registry` (`elab_toplevel.c:981`), so a program with one
  `(load)` reaches the install path twice on the same strict registry. Without
  the guard that is a hard `reader macro '#s' already registered` error on
  every loading program.

Put a comment at the guard saying so. It is the kind of line a later cleanup
deletes as redundant.

Do **not** add `{ "s", '"' }` to `kReserved` (`reader_macros.c:20-32`).
Registering rather than reserving keeps `#s"` overridable in a REPL session
(the registry is non-strict there, so a user `reader-macros/define` updates in
place, matching `defn` redefinition semantics), and it keeps the change to one
moved function. Reserving it would additionally turn any pre-existing
`#use-reader-macros` of the old file into a *different* error message
("cannot shadow built-in"), which is a worse landing than D2's no-op file.

Free win to note in the changelog: the native `tur lsp`
(`src/lsp/*.c`) never called `detect_lang_layered` at all, so `#s"..."` in a
`#lang turmeric stringed` file has always been a red squiggle in an editor.
Always-on registration fixes that with no LSP change.

### D2 -- Retire `stdlib/string-reader.tur`

Once `#s"` is registered at reader init, `#use-reader-macros
"stdlib/string-reader.tur"` re-registers the same `(name, delim)` pair against
the strict batch registry and hard-errors with a "previously registered here"
note (`reader_macros.c:157-166`). A working file becomes a compile error. This
must land in the same PR as D1.

Gut the file to comment-only: replace the `(reader-macros/define 's :string
'(string/from-cstr $body))` line (`string-reader.tur:28`) with a `;;;` note
saying `#s"..."` is now unconditional and the directive is unnecessary. Keep
the module docstring, rewritten. A file in the wild naming it keeps compiling
and its reader learns why.

Delete the file itself at age-out (D9), not now.

### D3 -- Delete the layer axis from the compiler core

Order matters: D1 must be in place first, or `#s"` breaks between commits.

1. Delete `src/compiler/lang_layers.{c,h}` and the `src/CMakeLists.txt:193`
   entry.
2. Collapse the two detection entry points into one. `detect_lang_layered`
   goes; `detect_lang` grows the bad-token out-params:
   ```c
   /* Detect a `#lang` directive.  `out_rest`/`out_rest_len` advance past the
    * directive line.  A trailing token after the base name is not legal --
    * `*out_bad`/`*out_bad_len` point at the first one (into `src`) and the
    * caller reports TUR-E0330.  Any out-param may be NULL. */
   ReaderType detect_lang(const char *src, size_t len,
                          const char **out_rest, size_t *out_rest_len,
                          const char **out_bad, size_t *out_bad_len);
   ```
   Four call sites: `main.c:188`, `main.c:7210`, `pkg.c:689`,
   `elab_toplevel.c:952`, plus `reader.c`, `eval.c`, `repl.c`, `wasm_glue.c`,
   `wasm_lsp.c`. The token loop keeps consuming to end-of-line -- that
   behavior is why the directive never leaks into the body and is independent
   of what the tokens mean.
3. Inside the loop, replace the three-way classify
   (`lang_layer_index` / `lang_layer_is_graduated` / bad) with a two-way:
   `lang_token_is_retired()` (D3.4) or bad.
4. Add the migration shim, self-contained, next to `detect_lang` in
   `reader.c`:
   ```c
   /* Tokens that used to be legal `#lang` layers.  The layer axis was
    * decommissioned in v0.45.0 (docs/upcoming/lang-layers-decommission-plan.md);
    * `stringed` is accepted and ignored for one minor line so a file that
    * opted in per-file keeps compiling across the boundary, exactly as
    * GRADUATED[] does for --enable.  Age out at 0.46.0. */
   static const char *const RETIRED_LANG_TOKENS[] = { "stringed", NULL };
   ```
   with a one-shot `TUR-W0064` warning, reusing the wording style of
   `lang_layer_is_graduated` (`lang_layers.c:121-137`). Six lines, no
   registry, no descriptor struct -- the migration window without the axis.
5. Reword the error. Keep the code `TUR-E0330` (same failure, same class) but
   say what is actually wrong now:
   ```
   error [TUR-E0330]: `#lang` takes a single base dialect; unexpected trailing
   token 'boguslayer' in path/to/file.tur
   ```
   Update the three emitters: `main.c:193-199`, `eval.c:11991-11995`,
   `elab_toplevel.c:956-961`, and the REPL's plain-text message
   (`repl.c:1699-1701`).
6. Delete `SourceFile.lang_layers` (`diag.h:442`), the `LangLayerSet` typedef
   (`diag.h:400`), and every assignment: `main.c:948, 1163, 1270`,
   `elab_toplevel.c:974`, `eval.c:12080`, `wasm_lsp.c:159`.
7. Delete `g_manifest_experiments_scoped` (`globals.c:290`, `globals.h:409`)
   and its only writer (`main.c:3004`). Its only reader was the semantic-layer
   check. Leave `m->has_experiments_key` and the manifest-scoping *behavior*
   for experiments alone -- that is `experiments.c`'s business and is
   unchanged.
8. Scrub the stale cross-references in `experiments.c:188` and
   `experiments.c:381` (both name `GRADUATED_LAYERS[]`).

### D4 -- CLI

Delete `cmd_lang_layers` (`main.c:10522-10578`), its dispatch
(`main.c:12134-12135`), its name-table entry (`main.c:9072`), and its help
line (`main.c:9136`). Remove the four `completion.c` sites (73, 132, 250, 284)
-- note 73 is a zsh `_call_program` that shells out to `tur lang-layers`, so
leaving it behind means a completion that silently produces nothing.

Removing a subcommand is a user-visible CLI break, so this ships in a minor
(0.45.0), not a patch.

### D5 -- Interpreter / REPL

- `turi_env_apply_lang(env, rt)` loses the `layers` parameter (`env.h:583`,
  `env.c:605`). **Keep the registry wipe.** Its comment currently justifies it
  by layers, but the real reason survives: `turi_env_reset_to_prelude` discards
  the session source, so the `#use-reader-macros` forms that registered user
  macros are no longer part of the session either. Rewrite the comment to say
  that, and note that D1's builtins are reinstalled on the next read.
- `eval.c:12010` -- the `env->lang_layers |= layers` union goes; the
  surrounding base-change reset stays.
- `repl.c:1694-1706` -- `detect_lang` with the new signature; keep the
  bad-token branch with the reworded message.

### D6 -- Try Turmeric and the wasm bridge

**C side.**

- `turi_wasm_set_lang` (`wasm_glue.c:409`): the argument becomes a base name
  only. A trailing token still returns 1 (rejected, environment untouched) --
  now because it is not legal, not because it is an unknown layer. Update the
  doc comment (`wasm_glue.c:392-408`), which currently explains assign-vs-OR
  semantics for a set that no longer exists.
- `turi_wasm_lang_registry` (`wasm_glue.c:505`): drop the `"layers"` key
  entirely; the JSON becomes `{"bases":[...]}`. Update `wasm_glue.h:144`.

**JS side (`web/main.js`).** The picker becomes a single-column dialect
popover.

- `parseLangDirective` (1209): stop returning `layers`; a trailing token is
  now malformed input from the editor buffer. Keep it *parsing* the whole line
  so `body` is still correct -- the playground should not choke on a pasted
  file, and the wasm side will reject it with a status message.
- `LANG_REGISTRY_FALLBACK` (1240): drop the `layers: []` key.
- `renderLangMenu` (1391-1430): delete the layer-rendering block and the
  checkbox listeners.
- `onLangControlChange` (1434) / `reconcileLangPicker` (1449) /
  `setLangDirective` / `currentLangSelection`: drop `layers` from the
  selection shape.
- **Tolerate a stale `layers` key on the registry object.** A returning
  visitor can hold a cached wasm binary from before this release; new JS
  reading `reg.layers` must simply ignore it rather than render a control.
  Ignoring an unknown key is the default if the rendering block is deleted --
  the point is not to add a `layers`-shaped assertion in its place.

**Markup and CSS.**

- `web/try/index.html:118-127`: delete the second `.lang-menu-col` and the
  `#lang-layers` div; the popover collapses to one column. Change the button
  `title` (line 114) from "Language dialect &amp; layers" to "Language
  dialect".
- `web/styles.css:1827-1860`: `.lang-menu-cols` no longer needs to be a
  two-column grid; `.lang-row-disabled`, `.lang-row-empty`, and `.lang-chip`
  become unused. Delete what nothing references -- a screenshot check of the
  popover at desktop and at the 480px mobile breakpoint is enough
  verification, plus the existing `lang-picker.spec.js` assertions on button
  height and the mobile overflow entry.

**Tests and release mechanics.**

- `web/tests/lang-picker.spec.js:120-142`: delete the layer test. The
  remaining specs (round-trip, follows-active-tab, mobile overflow, button
  height) all still apply.
- `web/README.md:151`: rewrite the `set_lang` description.
- `web/public/sw.js:31` `CACHE_VERSION` is stamped per release by the
  release-cut skills; no manual edit, but **verify after deploy** -- a stale
  service-worker cache is the standard reason a Try Turmeric change appears
  not to have landed.
- `web/dist/**` and `web/public/docs-pack/**` are generated. Do not hand-edit;
  they are rebuilt by `tur run docs` + the site build. The doc-pack copies of
  `syntax-guide.html` / `reader-forms-guide.html` pick up D8 automatically.

**Other projects.** Nothing else consumes the layer surface. `turmeric-godot`,
`turmeric-playground`, `turmeric-smtlib-corpus`, and every
`turmeric-spices*` worktree were swept at plan time with zero hits. The
editor integrations (`editors/`, `emacs/`, `vim-syntax/`,
`vscode-syntax-ext/`) highlight `#lang` as a line but never enumerate layer
tokens, so they need no change; the vscode extension's Turmeric grammar is
worth one look for a `stringed` keyword pattern before landing.

### D7 -- Fixtures and harnesses

- `tests/wasm_glue_lang_unit.c` -- rewrite. Delete the layer-toggle round-trip
  (lines 65-77) and the `"name":"stringed"` registry assertion (line 90).
  Replace with: `#s"hi"` evaluates in a fresh env with no `#lang` line at all;
  the registry JSON has `bases` and no `layers` key; a trailing token is
  rejected. Update the file header comment, which currently advertises layer
  coverage.
- `tests/run-flags.sh:947-995` -- delete `lang-layer-toggle-off` (it asserts a
  dispatch turning *off*, which no longer happens) and retarget
  `lang-layer-same-set-no-reset` at a bare repeated `#lang turmeric` line, so
  the "identical directive is a no-op, bindings survive" property stays
  pinned. `reader-name-canonical` (975-988) is untouched.
- `tests/fixtures/string-reader-macro/` -- **must change in this PR** (D2).
  Drop the `#use-reader-macros` line; keep everything else. Its Map-key and
  `eq?` assertions are the broadest `#s` coverage in the suite. It now pins
  the post-decommission idiom: `(load)` and nothing else.
- `tests/fixtures/lang-layer-stringed/` -- drop the token from line 1, rewrite
  the comment, rename to `string-literal-owned` (or fold into
  `string-reader-macro`, which it now duplicates almost exactly -- prefer
  folding and deleting one).
- `tests/fixtures/lang-layer-stringed-sweet/` -- drop the token, keep the
  fixture and rename (e.g. `string-literal-owned-sweet`). Do not delete it:
  it is the only thing pinning `#s"` under a non-default base, and D1 moves
  the install to a code path shared by both bases.
- `tests/fixtures/lang-trailing-tokens/` -- during the migration window it
  keeps `#lang turmeric stringed` and now pins the *retired-token* path
  (accepted, warned, stripped). Add a comment saying so, and add
  `expected.stderr` coverage of the TUR-W0064 line if the harness compares it.
  Retire the fixture at age-out (D9): with no legal trailing token, "stripped
  but not leaked" has no positive case left.
- `tests/fixtures/errors/lang-layer-unknown/` -- rename to
  `errors/lang-trailing-token`, update `expected.diag` to the reworded
  message. Keep `TUR-E0330`.
- `tests/fixtures/errors/lang-layer-retired-name/` -- it pins `#lang turmeric
  refined` as a hard TUR-E0330. Still correct; rewrite the 18-line comment,
  which is entirely about a shim table that no longer exists.
- `tests/fixtures/refine-graduated-lang-layer-noop/` -- delete the directory.
  It holds only `actual.*` / `turi.*` artifacts with no `input.tur`, left over
  when the fixture it replaced was removed.
- Regenerate `expected.c` snapshots if anything moves them (it should not --
  no codegen change here). Per CLAUDE.md, any regen lands in this PR.

### D8 -- Docs

- **`CLAUDE.md`** -- delete the "`#lang` Layers -- curated only" section
  outright. Replace with two or three lines under the syntax discussion:
  `#lang <base>` selects one base dialect; there is no layer axis; a one-off
  syntax convenience belongs in a `#use-reader-macros` file; a semantic gate
  belongs in `EXPERIMENTS[]`. Keep it short -- the value of the deleted
  section was its anti-proliferation process, and the process is now "there is
  no table to add to."
- **`docs/guides/syntax-guide.md`** -- rewrite Part 2.5 (497-580) as
  base-dialects-only; drop the layer sentence at 19-21, the "layers apply
  either way" clause at 370, and the `tur lang-layers` pointer at 577. Note
  the heading text feeds a site anchor
  (`#part-25----lang-base-dialects-and-layers`), so the rename is a docs
  rebuild, and any other guide linking that anchor needs updating.
- **`docs/guides/reader-forms-guide.md:262-281`** -- the heading "Owned-String
  literal -- `#s"..."` (layer `stringed`)" loses its parenthetical; same
  anchor caveat (`#owned-string-literal-s-layer-stringed`).
- **`docs/guides/strings-guide.md:123-152`** -- the "opt-in" section becomes
  "`#s"..."` is always available; `(load "stdlib/string.tur")` brings in the
  code it expands to." Delete the two-directive explanation; keep the
  read-time/eval-time paragraph, which is still the reason `(load)` is needed
  and is genuinely non-obvious.
- **Archive banners** -- one line at the top of
  `docs/archive/lang-layers-plan.md` and
  `docs/archive/try-turmeric-lang-toggle-plan.md`: superseded by this plan,
  with the version that decommissioned the axis. Do not rewrite their bodies;
  they are the historical record of why the axis existed.
- **Guide links to this plan use the GitHub URL**, not a relative path --
  only `docs/guides/` and `docs/api/` are published, so a relative link to
  `docs/upcoming/` 404s on the site.
- **`CHANGELOG.md`** under 0.45.0: the removal, the always-on `#s"`, the
  `tur lang-layers` deletion, the TUR-W0064 window and when it ends, and the
  reworded TUR-E0330.
- Move this plan to `docs/archive/` when it lands.

### D9 -- Age-out (a later release, not this one)

One minor line after the release that ships D1-D8 -- 0.46.0 if this is
0.45.0. Per CLAUDE.md this is advisory: it is a deadline, not a gate, and
nothing about it blocks a release cut.

1. Empty `RETIRED_LANG_TOKENS[]` (or delete it and the `TUR-W0064` emitter
   with it). `#lang turmeric stringed` becomes the same TUR-E0330 any trailing
   token gets.
2. Delete `stdlib/string-reader.tur`.
3. Retire `tests/fixtures/lang-trailing-tokens/`; add a `stringed` case to
   `errors/lang-trailing-token`.
4. Delete the "was a layer" paragraphs the D8 guides keep as migration notes.

---

## PR shape

**One PR, ordered commits.** D1+D2 (always-on `#s"`, file gutted) then D3-D6
(axis deleted) then D7+D8. The tempting split -- graduate `stringed` first,
delete the axis later -- ships an intermediate release in which `LANG_LAYERS[]`
is empty, which is a state with its own problem: `static const T X[] = {};` is
a GNU/Clang extension, and `lang_layers_count()` computes
`sizeof/sizeof` on it (`lang_layers.c:140`). Working around that with a
sentinel row is real code written only to be deleted a release later. Deleting
the table outright never has an empty-table state.

The ordering constraint within the PR is hard: D3 removes the only thing that
registers `#s"` today, so D1 must precede it or `#s` breaks between commits
and `git bisect` lands in a broken window.

Suggested commit sequence:

1. `reader: install #s"..." unconditionally` (D1) + `stdlib: retire
   string-reader.tur to a no-op` (D2) + the `string-reader-macro` and
   `lang-layer-stringed*` fixture updates. Green suite here on its own.
2. `lang: decommission the #lang layer axis` (D3, D5) + fixture/harness
   updates.
3. `cli: drop tur lang-layers` (D4).
4. `web: single-column dialect picker` (D6).
5. `docs: #lang takes a base dialect and nothing else` (D8) + CLAUDE.md.

## Risks and things that bite

Ranked by how likely they are to be discovered late.

1. **`#use-reader-macros "stdlib/string-reader.tur"` goes from working to a
   hard error the moment D1 lands.** The strict batch registry treats a
   duplicate `(name, delim)` as fatal with a "previously registered here" note.
   `tests/fixtures/string-reader-macro` catches it in-tree; a user file does
   not. This is why D2 is not optional and why the gutted file stays for a
   release.
2. **The install-idempotence guard is load-bearing across `(load)`, not just
   in the REPL.** `elab_toplevel.c:981` shares one strict registry with every
   loaded file, so install runs once per read call on the same registry.
   Dropping the `reader_macros_lookup` early return breaks every program that
   uses `(load)` -- which is most of them.
3. **`tests/wasm_glue_lang_unit.c` fails at the link/run step, not the
   compile step**, and it is a separate ctest target
   (`CMakeLists.txt:1324`), so a `bash tests/run.sh`-only check will not see
   it. Run it explicitly.
4. **Service-worker cache on Try Turmeric.** After deploying, verify against a
   hard reload and against a returning-visitor path. `CACHE_VERSION` drift is
   the usual reason a web change looks like it did not land.
5. **`detect_lang` signature churn touches `pkg.c` and `tur fmt`**, both of
   which only want the strip. Passing NULL for the new out-params keeps them
   behaviorally identical -- but `pkg.c` reads *manifests*, and a manifest with
   a trailing `#lang` token would now be silently tolerated there and rejected
   elsewhere. Decide deliberately: recommended is to pass the out-params and
   diagnose, so `build.tur` and `build.tur.sweet` follow the same rule as
   source.
6. **Anchor rot in the published guides.** Two headings change
   (`#part-25----lang-base-dialects-and-layers`,
   `#owned-string-literal-s-layer-stringed`). Grep `docs/guides/` and
   `web/` for both before landing.
7. **Nothing outside this repo depends on the axis** -- verified, but re-run
   the D0 sweep at landing time. The spices repo moves independently.

## Verification

- `bash tests/run.sh 2>&1 | tee /tmp/run.log` with a 12-minute timeout
  (`timeout: 720000`), reading `^FAIL` lines. Expect ~1442 fixtures, ~4-5 min.
  Do not run it concurrently with a build.
- `bash tests/run-turi.sh` (same timeout) -- `lang-trailing-tokens` and both
  string fixtures have `turi.stdout` baselines, so the interpreter path is
  genuinely exercised.
- `bash tests/run-flags.sh` -- the REPL `#lang` assertions.
- `ctest -R tur_wasm_glue_lang_unit` (`RUN_SERIAL`; do not oversubscribe).
- `cd web && npx playwright test tests/lang-picker.spec.js`.
- By hand, in a scratch file with **no** `#lang` line:
  ```turmeric
  (load "stdlib/string.tur")
  (defn main [] : int (println (string/to-cstr #s"hi")) 0)
  ```
  `./build/tur run` it, `./build/tur build` it, and `./build/tur --interpret`
  it. That three-way check is the whole point of the change.
- `printf '#lang turmeric stringed\n(defn main [] : int 0)\n' > /tmp/w.tur` --
  exit 0 with one TUR-W0064 line during the window.
- `printf '#lang turmeric nosuch\n' > /tmp/e.tur` -- TUR-E0330 with the new
  wording, on the compiled path, `--interpret`, and in `tur repl`.
- `./build/tur lang-layers` -- "unknown command", and shell completion for
  `tur <TAB>` no longer offers it.

Use `./build/tur` throughout, never a PATH `tur` -- a stale Homebrew install
will report the old behavior and cost an afternoon.

## Summary

| | Before | After |
|---|---|---|
| `#lang` grammar | `#lang <base> <layer>*` | `#lang <base>` |
| `#s"..."` | needs `stringed` or `#use-reader-macros` | always on; `(load "stdlib/string.tur")` for the code |
| Layer registry | `LANG_LAYERS[]`, `GRADUATED_LAYERS[]`, `LangLayerSet` bitset in 4 paths | gone |
| Semantic gates | layer *or* `--enable` | `--enable` only |
| CLI | `tur lang-layers [--json]` | gone |
| Try Turmeric picker | dialect radios + layer checkboxes | dialect radios |
| Trailing token | TUR-E0330 unless registered | TUR-E0330 always (`stringed` warned for one line) |
