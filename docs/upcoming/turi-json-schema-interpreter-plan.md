# turi: json + schema under `--interpret` (interpreter gap-closure)

> **Status:** DONE (2026-06-13). Both layers landed -- json and schema run
> under `--interpret` via layout-exact natives in `src/main.c`
> (`wk_register_json_natives` / `wk_register_schema_natives`); the 5
> `json-reader-*` and 16 runnable `schema-*` fixtures are on the
> `run-turi.sh` allowlist (the 2 `schema-applicative-user*` fixtures stay
> carved -- they define their own inline-C). A `tur-vec-homog__` no-op native
> was also needed so the `vec-of` macro works under `--interpret`. The
> captured inventory + approach below is retained for reference.
> **Tracking report:**
> [docs/reported/turi-harness-flip-reconciliation.md](../reported/turi-harness-flip-reconciliation.md)
> (the W5 allowlist->denylist surface).
> **Sibling plan:**
> [turi-interpreter-gap-closure-plan.md](turi-interpreter-gap-closure-plan.md)
> (W1-W5 workstreams; this is the json/schema slice of W5's residual inline-C tail).

## Summary

`stdlib/json.tur` and `stdlib/schema.tur` are the last large fixture cluster
that does not run under `--interpret`: **19 `schema-*` fixtures** plus **5
`json-reader-*` fixtures** all fail with `inline-C not supported`. Both modules
are intentionally carved from the interpreter prelude
(`docs/turi-preload-carve-out.txt`: "reader-backed, -X-gated"), and unlike the
already-landed sym/seq work they are **not a thin bridge layer** -- they are a
self-contained C engine: a tagged-AST JSON value model + recursive-descent
parser + encoder (30 inline-C functions), and a schema combinator/decoder
runtime layered on top (40 inline-C functions). Closing the gap means
re-implementing that engine as interpreter natives with **layout-exact** node
structures so the cross-function calls line up.

This is a dedicated, multi-step effort (its own PR), materially larger than the
sym (5 natives) or seq (~25 thin bridges) work. It splits cleanly into two
independently-landable layers; **do the json layer first** -- it is the
foundation schema builds on, and it independently unblocks the 5 `json-reader-*`
fixtures.

## Why it is gated today

- `json.tur` / `schema.tur` are not in the `cmd_eval` prelude
  (`tools/check_turi_native_parity.py` lists them as carved preload gaps).
- Their public ops are inline-C wrappers over malloc'd C structs and a
  recursive-descent parser (`strtoll`/`strtod`/`strncmp`/`memcpy`), which
  `try_exec_simple_inline_c` cannot run.
- The fixtures `(load "stdlib/json.tur")` + `(load "stdlib/schema.tur")`
  explicitly, so the fix is **native overrides** (registered at startup; they
  win over the loaded inline-C defns via the override hook at
  `src/turi/eval.c` ~`:3862`), not a prelude change.

## The proven pattern (from sym/seq this session)

1. **Native overrides win over loaded inline-C.** Register a native under the
   exact function name; when the loaded inline-C defn is called, the dispatcher
   at `src/turi/eval.c:3862` looks up the native by name and runs it instead.
2. **Consistent layouts.** Nativize every producer AND accessor of a struct
   together so the malloc layout is owned end-to-end (seq did this for its
   option/out-vec/cons/Tuple2 + gen-arr helpers). Match the inline-C layout
   exactly where any non-nativized reader might still touch it.
3. **Fat-closure callbacks.** A `^fat f : int` callback arrives as a
   `TURI_CLOSURE` (its `.as_int` aliases the closure pointer); call it with
   `turi_call(env, f_value_or_seq_as_closure(f), args, n)`. See
   `seq_as_closure` / the `seq-call-*` natives in `src/main.c`.
4. **Generators / recursion** can be driven from a native via `turi_call` and
   (for gen bodies) the public `turi_gen_advance_val`; deep recursion in a
   parser is fine in C.
5. **Carve honestly.** A fixture with its OWN `` ```c `` block is an inline-C
   carve-out (auto-skipped). A genuine interpreter/compiler divergence gets a
   `requires.compiled` / `requires.tur-only` marker with a one-line reason --
   never rewrite the fixture to dodge the breakage.

## Layer 1 -- json (do first; also unblocks `json-reader-*`)

### Node layout (already fully specified in `stdlib/json.tur`)

A JSON node is `int64_t[2] = {type, payload}`:

| type | meaning | payload |
| --- | --- | --- |
| 0 | null | 0 |
| 1 | bool | 0 / 1 |
| 2 | int | the int64 value |
| 3 | float | the double bits (`memcpy` into the int64 slot) |
| 4 | string | `char*` (strdup'd) |
| 5 | array | `struct { int64_t *data; size_t len; size_t cap; }*` |
| 6 | object | head of a linked list of `int64_t[3] = {char* key, int64 val, int64 next}` (LIFO; later puts prepend) |

(Source: `json/null`/`bool`/`int`/`float`/`string` `:41-133`, `json/array-new`
`:134`, `json/object-put` `:199`.)

### Functions to nativize (`stdlib/json.tur`, 30 inline-C)

- **Builders:** `json/null` `:41`, `json/bool` `:60`, `json/int` `:79`,
  `json/float` `:98`, `json/string` `:118`, `json/array-new` `:134`,
  `json/array-push` `:156`, `json/object-new` `:178`, `json/object-put` `:199`.
- **Accessors:** `json/type` `:222`, `json/get-bool` `:240`, `json/get-int`
  `:257`, `json/get-float` `:274`, `json/get-string` `:293`, `json/array-len`
  `:310`, `json/array-get` `:330`, `json/get` `:352` (object lookup by key),
  `json/get!` `:383`.
- **Encoder:** `json-enc-ensure-`/`-append-s-`/`-append-c-`/`-str-`/`-node-`
  `:413-457`, `json/encode` `:502`. (A growable char buffer + recursive node
  walk; can be one native that recurses in C.)
- **Decoder (recursive-descent):** `json-dec-skip-ws-` `:530`,
  `json-dec-parse-string-` `:539`, `json-dec-parse-value-` `:573` (recursive;
  handles null/true/false/string/number/array/object over a
  `struct { const char *s; size_t pos; int err; }` context), `json/decode`
  `:678`.
- **Free:** `json-free-node-` `:697`, `json/free` `:723` (no-ops are fine under
  the interpreter's process-lifetime policy, but match the signature).

The cleanest implementation is a small `json_native.c`-style block in
`src/main.c` (next to the seq natives): the parser and encoder recurse in C
exactly as the inline-C does; the float payload uses a `union { int64; double }`
to match the `memcpy` bit-encoding. `json/get-float` and `json/float` are the
float-aware ones (probe with `7.1`, never `7.0`, per CLAUDE.md).

### Validation (Layer 1)

`json-reader-array`, `json-reader-escape`, `json-reader-nested`,
`json-reader-null`, `json-reader-object` must pass under `--interpret` and join
the `run-turi.sh` allowlist. These use only the json layer (no schema).

## Layer 2 -- schema (depends on Layer 1)

### Value model

- **Schema node:** combinators (`schema/str` `:100` ... `schema/literal-str`
  `:193`, `schema/object-new` `:217`, `schema/field` `:239`, `schema/array`
  `:269`, `schema/optional` `:289`, `schema/union` `:309`, `schema/transform`
  `:331`, `schema/rec` `:364`, `schema/ap` `:466`, `schema/fmap` `:513`, ...)
  build a tagged int schema representation. Inventory the tag layout the same
  way (read each builder's `` ```c ``).
- **Result:** `schema-decode` `:990` returns
  `struct { bool is_ok; int64 ok_val; int64 err_val; }*`; `schema-decode-ok?`
  `:1018`, `-value` `:1033`, `-errors` `:1048` read it. Error list is a
  `tur_sch_vec_t {data,len,cap}` of error records.
- **Errors:** `schema-error-path` `:630`, `-text` `:645`, `-count` `:660`,
  `-at` `:676`, `-message` `:696`; internal `sch-push-err-` `:759`,
  `sch-mkpath-` `:768`, `sch-mkidx-` `:776`.

### The core

`sch-decode-rec-` (`:815`-`:990`, ~175 lines of inline-C) is the recursive
decoder: walks (schema node, json node, path) and accumulates errors into the
`errs` vector. It dispatches on the schema tag, recurses for object fields /
array elements / unions / optionals, and invokes the `schema/transform` /
`schema/fmap` fat closures. The fat-closure calls (`schema/transform`'s `f`,
`schema/ap`'s applicative, the `Functor`/`Applicative`/`Alternative [Schema]`
instances `:594-610`) are the parts that **must** route through `turi_call`
(the compiled C-fptr call is the blocker, same shape as seq's `seq-call-*`).

### Validation (Layer 2)

The 19 `schema-*` fixtures (all flagless except `schema-reader-json-str-runtime`
which needs `-Xschema-reader`). Add the passing ones to the allowlist; carve any
that genuinely cannot run (e.g. if `-Xschema-reader`'s reader macro is
interpreter-incompatible, mark `requires.compiled` with a reason).

## Sequencing

1. **Layer 1 (json) natives** + `json-reader-*` on the allowlist. Self-contained,
   independently valuable, no schema dependency. ~30 small natives + 1 recursive
   parser + 1 recursive encoder.
2. **Layer 2 (schema) natives** building on the json nodes: schema-node
   builders, the `sch-decode-rec-` recursive decoder (the big one), the
   Result/error model, and the fat-closure bridges for transform/fmap/ap.
3. Once both land, consider whether `json.tur`/`schema.tur` can leave
   `docs/turi-preload-carve-out.txt` (preload behind `TUR_TURI_FULL_PRELUDE`
   already loads them; full preload depends on the `-X` reader gates).

## Risks / watch-items

- **Layout exactness.** Every native producer/accessor pair must agree
  bit-for-bit; the float payload (`memcpy` double into an int64 slot) and the
  object entry list (LIFO `{key,val,next}`) are the easy-to-get-wrong spots.
- **Error-accumulation semantics.** `schema-decode` returns ok iff the error
  vector is empty after the full walk; applicative decoders accumulate multiple
  errors (`schema-applicative-error-accumulation`). Drive the order to match the
  compiled output exactly.
- **`-Xschema-reader`.** `schema-reader-json-str-runtime` uses a reader macro;
  if the reader path is interpreter-incompatible it is a clean
  `requires.compiled` carve, not a fix.
- **Validation cost.** 24 fixtures + the full `run-turi.sh` (must stay green) +
  the compiled `run.sh` (1606/0) + both parity gates each step.

## Definition of done

`json-reader-*` (5) and the runnable `schema-*` (most of 19) pass under
`--interpret` and are on the `run-turi.sh` allowlist; any residual fixtures
carry an honest `requires.*` marker with a one-line reason; `run.sh` and both
turi parity gates stay green; no codegen change (interpreter-only).
