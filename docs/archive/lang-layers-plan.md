# `#lang` Layers -- base dialect + additive feature layers

Status: L0-L3 + L5 landed (2026-07-23). L4 (semantic-layer bridge / `refined`)
still rides the refinement-types work -- the LANG_LAYERS[] row struct already
carries the `LAYER_SEMANTIC` kind + `experiment` field for it, but no semantic
layer is registered until its backing EXPERIMENTS[] row exists.
Introduced: 2026-07-23

## What landed

- `src/compiler/lang_layers.{h,c}` -- the curated `LANG_LAYERS[]` registry
  (`LangLayerSet` bitset, `LangLayerKind`, lookup/count/at, and
  `lang_layers_apply_readers`). One row today: the `stringed` reader layer.
- `detect_lang_layered` parses the base + the trailing layer set, consumes to
  EOL (the old trailing-token leak is gone), and reports an unknown layer via
  the `out_bad` token; `detect_lang` is now a base-only wrapper over it.
- `SourceFile.lang_layers` + `TuriEnv.lang_layers` carry the set through the
  compiled build (`main.c`), the `--interpret` path (`eval.c`), and the REPL
  (`repl.c`). `read_all_with_registry` runs each enabled reader layer's hook
  before the first form.
- `stringed` => `#s"..."` owned-String literal, built-in and curated (same
  expansion as `stdlib/string-reader.tur`, no `#use-reader-macros` needed).
- `turmeric/sweet` base accepted; `sweet-exp` kept as a legacy alias.
- Unknown layer token => hard error `TUR-E0330` (compiled, interpret, REPL).
- `tur lang-layers [--json]` lists the registry. Docs in
  `docs/guides/syntax-guide.md` (Part 2.5) and `reader-forms-guide.md`.
- Fixtures: `lang-layer-stringed`, `lang-layer-stringed-sweet`,
  `errors/lang-layer-unknown`.

## Motivation

Three unrelated mechanisms currently govern "what dialect / extra syntax /
extra semantics does this file get":

1. **Base reader selection** -- `#lang` line 1, one name token, single
   `ReaderType` (`src/compiler/reader.c:4035` `detect_lang`). Accepts exactly
   `turmeric`, `turmeric/curly-infix`, `turmeric/neoteric`, `sweet-exp`.
2. **User reader macros** -- `#s"..."` and friends, activated mid-file by
   `#use-reader-macros "path"` (`reader.c:3126`). Fiddly relative-vs-`base_dir`
   path resolution, a `stdlib/` retry fallback, a synthetic `<eval>` file slot,
   and a known transitive-coverage gap
   (`docs/archive/reader-macros-transitive-plan.md`).
3. **Semantic feature gates** -- e.g. refinement checking behind
   `-Xrefinements` / the `--enable=<name>` experiments registry
   (`src/runtime/experiments.c`). The `#refine{...}` *reader* is already
   always-on; only the checker is gated.

This plan unifies (1)-(3) behind one first-line directive:

```
#lang <base>[/<dialect>] <layer>*
#lang turmeric/sweet stringed refined
```

A file becomes self-describing: everything that changes how it reads or checks
is declared on line 1, read before any form, guaranteed file-scoped.

### Why this is worth doing (not just cosmetic)

`#lang` is parsed before the first form, so a **reader-layer** flag declared
there is active for the *entire* file -- which `#use-reader-macros` cannot
cleanly promise (it appears between top-level forms, only affects what follows,
and has the transitive gap). Hoisting curated reader-macro activation to a
closed, first-line set retires the path-resolution wart for the common cases.
Arbitrary user macro bundles stay on `#use-reader-macros`; `#lang` layers are a
**curated, registered** set only.

## Model (agreed)

- **Base** is one of the existing mutually-exclusive readers, selected by the
  slash-namespaced name: `turmeric`, `turmeric/curly-infix`,
  `turmeric/neoteric`, `turmeric/sweet` (new spelling of `sweet-exp`). These do
  not compose -- sweet-exp is a whole indentation preprocessor pass, curly /
  neoteric are flags on the same base reader. Base stays a single `ReaderType`.
- **Layers** are the space-separated trailing tokens. They are an
  **order-independent set**, not a pipeline. Each layer is one of two kinds:
  - **Reader layer** -- flips on a `#`-dispatch (e.g. `stringed` => the
    `#s"..."` owned-String literal). Additive and commutative by construction.
  - **Semantic layer** -- flips on an elaboration/checker gate (e.g. `refined`
    => refinement checking). Desugars to the *same* enable-set as
    `--enable=<name>`.

Slash = pick a base (mutually exclusive). Space = add a layer (set union). This
reconciles with the existing `turmeric/curly-infix` convention and reads in the
Racket "`#lang <meta> <base>`" direction.

## Current-state details that constrain the implementation

- `detect_lang` today reads **only the first name token** and leaves
  `out_rest` pointing at the *rest of the line* (the space after the name
  onward, including the newline and the whole body). So `#lang turmeric
  stringed` today does **not** strip `stringed` -- it leaks into the body and
  the reader tries to read `stringed` as a top-level symbol. The layer parser
  must therefore **consume to end-of-line** after collecting layer tokens.
- Unknown base returns the `(ReaderType)-1` sentinel (`reader.c:4104`), checked
  by `reader_type_is_implemented` (`reader.c:4136`). Unknown *layer* tokens need
  their own hard error (new diagnostic, see below), not silent drop.
- The base `ReaderType` is threaded through ~6 sites: file compile
  (`src/main.c:140` `detect_and_adjust_lang`, callers at `main.c:788/984/1082`,
  forced selection `main.c:5446`, prelude pre-detect `main.c:5977`), REPL
  (`src/turi/repl.c:1246`, `env->reader_type` `src/turi/env.h:255`), interp
  (`src/turi/eval.c:9758`), WASM (`src/web/wasm_glue.c:388`). Layers must ride
  alongside, not replace, that value.
- Shebang-above-`#lang` is **already supported** (`reader.c:4040`,
  `eval.c:9766`, fixtures `shebang-tur`, `shebang-sweet-lang`). Out of scope
  here; do not re-touch it.

## Design

### Grammar

```
langline   := shebang? WS* "#lang" WS+ base (WS+ layer)* WS* EOL
base       := "turmeric" ("/" dialect)?
dialect    := "curly-infix" | "neoteric" | "sweet"
layer      := <ident from the layer registry>
```

Bare files (no `#lang` line) remain implicit `#lang turmeric` with no layers --
no flag day. The "base name is mandatory" rule applies only *when a `#lang`
line is present*.

`sweet-exp` stays accepted as a legacy alias of `turmeric/sweet`.

### Data model

- `detect_lang` gains an out-param for the layer set (a small
  `LangLayerSet` -- bitset over the registry, or a `uint32_t` while the registry
  is small). Base stays `ReaderType`.
- New table `LANG_LAYERS[]` (co-located with the reader, e.g.
  `src/compiler/lang_layers.c`), one row per layer:

  | field        | meaning                                                     |
  |--------------|-------------------------------------------------------------|
  | `name`       | token as written in `#lang` (`"stringed"`, `"refined"`)     |
  | `kind`       | `LAYER_READER` \| `LAYER_SEMANTIC`                          |
  | `reader_hook`| (reader layers) fn that registers the `#`-dispatch          |
  | `experiment` | (semantic layers) the `EXPERIMENTS[]` name to enable        |
  | `summary`    | one line, for `--help`/docs                                 |
  | `since`      | version introduced                                          |

- **Reader layers** call their `reader_hook` at reader init, before the first
  form -- for `stringed` this registers the same string macro
  `stdlib/string-reader.tur` installs (`(string/from-cstr ...)` expansion),
  but built-in and curated rather than file-loaded.
- **Semantic layers** enable `experiment` through the *existing* experiments
  path: same `EXPERIMENTS[]` row, same `experiment_warn_if_used` lifecycle
  (TUR-W0060/W0061), same `expires_at` contract. `#lang turmeric refined` is
  exactly `--enable=refined` scoped to one file. No parallel flag system.

### Precedence (agreed: hard error, no silent-ignore)

A `#lang` layer is a *hard requirement of the file* -- the file will not read
or check correctly without it. So for a semantic layer whose experiment the
project has disabled (manifest `:experiments []` or an omitted `:enable`):

- Do **not** silently honor and do **not** silently ignore.
- Emit a hard error: `this file requires layer 'refined' (#lang), which is
  disabled by the project manifest`. New diagnostic, e.g. **TUR-E02xx**
  "lang-layer disabled by manifest".

Unknown layer token => hard error **TUR-E02xx** "unknown #lang layer 'foo'"
(list known layers). Reader layers have no manifest interaction -- they either
exist in the registry or they don't.

CLI `--enable=<name>` and a `#lang` semantic layer are additive; neither
suppresses the other. The manifest suppression rule is the only conflict, and
it resolves to the hard error above.

## Phasing

- **L0** -- Layer registry scaffold (`LANG_LAYERS[]`, empty), `detect_lang`
  parses base + layer tokens, **consumes to EOL**, returns the layer set;
  unknown-layer hard error. No layers registered yet, so behavior is identical
  except the leaked-trailing-token bug is fixed.
- **L1** -- Thread the layer set through the ~6 `ReaderType` sites +
  `env->reader_type` neighbor field. REPL `#lang` re-parse handles layers.
- **L2** -- First reader layer: `stringed`. Register the string macro from a
  built-in hook; `#lang turmeric stringed` gives `#s"..."` with no
  `#use-reader-macros`. Fixtures: bare, with-sweet, unknown-layer error.
- **L3** -- `turmeric/sweet` accepted as base; `sweet-exp` kept as alias.
  Docs + `.tur.sweet` extension unchanged (extension selects base only).
- **L4** -- Semantic-layer bridge to `EXPERIMENTS[]`; wire `refined` once
  refinement checking (RT-plan) is far enough. Manifest-disabled hard error.
- **L5** -- Docs (`syntax-guide.md`, `reader-forms-guide.md`), `--help` listing
  of layers, migration note for `#lang sweet-exp` -> `#lang turmeric/sweet`.

L0-L3 are landable without the refinement work; `refined` (L4) rides RT-plan.

## Anti-proliferation process (proposed CLAUDE.md addition)

Sole-consumer today, but `#lang` becoming a flag list is the one real risk
(feature-flag soup, non-commuting layers). Proposed short section to lift into
CLAUDE.md:

> ### `#lang` Layers -- curated only
>
> A `#lang` layer token is legal **only** if it has a row in `LANG_LAYERS[]`.
> Adding a layer means:
> - One `LANG_LAYERS[]` row with every field populated (`name`, `kind`,
>   `reader_hook` or `experiment`, `summary`, `since`).
> - Reader layers: the `#`-dispatch must be additive and commutative with every
>   other reader layer (no ordering dependence). If it isn't, it's a base
>   dialect (slash-namespaced), not a layer.
> - Semantic layers: **must** point at an existing `EXPERIMENTS[]` row -- never
>   a second, parallel enable path. The experiment carries the lifecycle and
>   `expires_at`.
> - A doc paragraph in `docs/guides/syntax-guide.md`.
>
> Prefer *not* adding a layer. A one-off syntax convenience belongs in a
> `#use-reader-macros` file, not the curated `#lang` set. Graduate a layer to
> always-on (delete the row, behavior unconditional) rather than letting layers
> accumulate.

(Landing this in CLAUDE.md is deferred until L0 exists, so the referenced
table is real.)

## Open questions

- `LangLayerSet` representation: bitset vs small array. Bitset is simplest while
  layers stay < 32; revisit if the set grows.
- Should `--help` / `tur --version` advertise available layers, mirroring
  `--enable` experiment listing? (Lean yes at L5.)
- Legacy `sweet-exp` alias: warn-and-accept, or accept silently? (Lean silent
  through v1, soft-deprecate later.)

## Out of scope

- Shebang handling -- already implemented and tested.
- Arbitrary user reader-macro bundles in `#lang` -- stays on
  `#use-reader-macros` to avoid reintroducing path resolution into line 1.
- Refinement type *checking* itself -- owned by
  `docs/upcoming/v1/refinement-types-plan.md`; this plan only provides the
  `#lang turmeric refined` front-door.
