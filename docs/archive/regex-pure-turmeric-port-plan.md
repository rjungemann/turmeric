# Pure-Turmeric regex engine (`stdlib/re.tur`) -- Plan

> **Status:** DONE (2026-07-13).
> **Last Updated:** 2026-07-19
> **Type:** stdlib / interpreter parity / WASM
> **Scope:** Replace `stdlib/re.tur`'s POSIX-`regcomp` inline-C with a
> pure-Turmeric matcher so regexes work under `tur --interpret` AND in the
> browser/WASM REPL -- where they are completely absent today despite the
> `#rx"..."` literal already parsing everywhere.
> **Depends on:**
> [interp-string-natives-and-range-show-plan.md](interp-string-natives-and-range-show-plan.md)
> Phase 0 (the `cstr-nth` / `cstr-len` / `str-concat` interpreter natives). Do
> that first -- without runnable byte access no pure matcher can read input.
>
> ---
>
> ## Resolution (2026-07-13)
>
> Shipped. `stdlib/re.tur` is now a pure-Turmeric backtracking engine with zero
> `` ```c `` blocks; it runs on the AOT backend, `tur --interpret`, and (via the
> shared interpreter path) the WASM REPL.
>
> **What landed vs. the plan below:**
> - **Phase 0 natives** added to `src/turi/interpreter_natives.c`: `str-concat`,
>   `cstr-len`, `cstr-nth`, plus a new `cstr-sub` (substring) both as an
>   interpreter native and an inline-C `stdlib/cstr.tur` counterpart. These are
>   the byte/string primitives the matcher reads input and builds output through.
> - **Phase 1 parser + Phase 2 matcher** implemented, but the engine strategy is
>   a **set-of-positions** matcher (`run-k`: match a Regex from a *set* of start
>   positions, returning every reachable end position), not the AST-index
>   backtracker sketched in the plan. This gives leftmost-longest per start (it
>   takes the max reachable end) and terminates on empty-matchable stars via a
>   dedup + fixpoint, with no separate NFA compile step.
> - **Handle typing:** `re/compile` returns a real `Regex` ADT value (the parsed
>   AST) directly -- NOT an `:int`. This is the only representation that survives
>   the interpreter, where an ADT is a `TURI_STRUCT` and cannot round-trip
>   through an `:int` opaque. `re/free` / `re/match-free` / `re/find-all-free`
>   are no-op shims.
> - **Phase 3 captures: deferred.** Group capture indices are not threaded, so
>   `re/match` returns the whole match as an `(Option cstr)` (leftmost-longest).
>   `re/find-all` returns an `RxStrs` cons list of matched substrings;
>   `re/replace` / `re/replace-all` do literal splicing. No consumer used the old
>   group-vec return, so this is a clean signature change, documented in the
>   module docstring.
>
> **Implementation constraints discovered (worth recording):**
> - Compiled ADTs are **linear/unique** -- a value used twice is an
>   `TUR-E0201`. The matcher reuses sub-expressions constantly, so `Regex`,
>   `RxCls`, and `RxPos` (the position list) are declared `:copy`.
> - A **single-constructor** ADT lowers to a by-value record; when it carries a
>   heap ADT field the match/bind path miscompiles ("aggregate value used where
>   an integer was expected"). Fixed by giving the wrapper types (`RxPair`,
>   `RxParse`) a dummy second variant so they lower to tagged heap handles.
> - **Forward references to an ADT-returning function default the callee's return
>   type to `int`** (the top-level pre-pass registers only scalar *param* kinds).
>   Genuine mutual recursion between ADT-returning functions therefore does not
>   type-check. Both the matcher and the recursive-descent parser are written as
>   **single self-recursive functions** (`run-k`; `re-parse-go` with a `mode`
>   dispatch) so every call is a self-call or a call to an already-defined leaf.
>
> **Validation:** `bash tests/run.sh` = 2124 passed / 0 failed;
> `bash tests/run-turi.sh` = 1595 passed / 0 failed (`reader-macros-rx-literal`
> now runs un-carved; new `re-pure-match-find-replace` fixture PASSes under
> `--interpret`). `re.tur` has zero `` ```c `` blocks. The
> `(re/match? #rx"^[0-9]+$" "123")` acceptance eval runs under `--interpret`, and
> `src/web/wasm_glue.c` registers the same interpreter natives, so the browser
> REPL gets it with no WASM rebuild.
>
> The Thompson-NFA upgrade and Phase 3 captures remain as documented follow-ups.
>
> **Verified 2026-07-19:** `stdlib/re.tur` still has zero `` ```c `` blocks; the
> Phase 0 natives (`str-concat`/`cstr-len`/`cstr-nth`/`cstr-sub`) are registered
> in `src/turi/interpreter_natives.c`; `tests/fixtures/reader-macros-rx-literal/`
> carries no `requires.tur-only` marker and the `re-pure-match-find-replace`
> interpreter fixture is present. Core port complete; only the two named
> follow-ups (Thompson-NFA, captures) remain and are out of this plan's tracked
> scope. Ready to archive.
>
> ---

---

## Motivation

Regexes are absent from the interpreter and the browser REPL, and the reason is
almost silly: **Turmeric owns no regex algorithm.** `stdlib/re.tur` is a thin
wrapper over the host libc's POSIX engine (`regcomp`/`regexec`/`regfree`,
`REG_EXTENDED`, `re.tur:32`). `re/compile` returns a boxed pointer to a
`malloc`'d `regex_t` (`re.tur:31`); every match op is inline-C calling into libc.
There is no `regex_t` in a WASM sandbox and no linked libc regex for the
tree-walker, so the whole module dies at the inline-C wall.

Meanwhile the ergonomic half is already done and already portable: the
`#rx"..."` reader literal is **built into the reader** (`reader.c:2625-2644`;
reserved in `reader_macros.c:31`), expands to `(re/compile "...")` before
elaboration, and works in every dialect on every path. Only the engine behind it
is missing. Porting the engine to pure Turmeric lights up regex in the
interpreter and -- for free -- in the browser.

**"Not having regexes in the browser when they're right there" is exactly this
gap, and it closes with a library rewrite plus three tiny natives.**

---

## What exists today

**Inline-C surface (9 blocks, all libc POSIX):** `re/compile` (:28),
`re/free` (:45), `re/match?` (:67), `re/match` (:92, builds a vec of `strdup`'d
group slices), `re/match-free` (:128), `re/find-all` (:152, cons list of
match-vecs), `re/find-all-free` (:196), `re/replace` (:229, literal replacement),
`re/replace-all` (:333).

**Already pure (but dead under `--interpret` because they lean on inline-C
`str-concat`):** `re/wrap-paren` (:247), `re/union-acc` (:253),
`re/union-patterns` (:288), `re/compile-union` (:313).

**Supported syntax (POSIX ERE):** anchors `^ $`, classes `[...]`/`[^...]` incl.
`[:alpha:]`, quantifiers `* + ?` and bounded `{n,m}`, alternation `|`,
capturing groups `(...)`, `.`. **Not** supported: backreferences, Perl
shorthands (`\d \w \b`), lookaround, non-greedy. Match semantics: POSIX
leftmost-longest.

**WASM availability mechanism (key):** the whole `stdlib/` tree is embedded into
the WASM VFS at link time (`src/CMakeLists.txt:865`,
`--embed-file .../stdlib@/stdlib`), and the browser REPL runs the SAME
tree-walking interpreter as `tur --interpret` (`wasm_glue.c:172`,
`turi_env_register_interpreter_natives`). **Consequence: the moment a
pure-Turmeric `re/match?` runs under `--interpret`, the browser gets it with no
build or glue change.** Single acceptance gate.

---

## Design decision: match semantics

The port is an opportunity to also fix the `:int`-as-eraser handle typing
(`re/compile` returns `:int`; the match vec and find-all list are `:int`). Give
them real types: `defopaque Regex` (or a `defdata` AST handle),
`vec<cstr>`/`list<cstr>` for results.

**Two engine choices:**

1. **Recursive-descent backtracking** over `(AST, input-index)` returning
   `option<int>` (end position). Simplest correct port; lowest friction in
   Turmeric (ADT + recursion + `option`/list backtracking); matches the current
   *unanchored, leftmost* search directly. Risk: pathological backtracking on
   adversarial patterns -- acceptable for a REPL/stdlib matcher.
2. **Thompson NFA** (compile AST -> state graph, simulate a set of active
   states). Linear-time, better POSIX leftmost-longest fidelity, no catastrophic
   backtracking. Needs a state set/worklist; keep it a plain sorted cons list
   (not `stdlib/set.tur`, whose ops are inline-C).

**Recommendation: ship (1) first** (a correct, small, fully-portable matcher),
leave the Thompson-NFA upgrade as a documented follow-up. Do not gate behind an
experiment -- this is a library replacement of an always-on module, not an
in-flight compiler feature.

---

## Phases

### Phase 0 -- interpreter string/byte natives (SHARED PREREQUISITE)

Provided by
[interp-string-natives-and-range-show-plan.md](interp-string-natives-and-range-show-plan.md):
`str-concat`, `cstr-nth`, `cstr-len` as layout-exact interpreter natives
(`interpreter_natives.c`, registered at `:3975`). These are the minimum needed to
read input bytes and build replacement output under the interpreter/WASM. Land
that plan's Phase 0 first; the existing `re/union-*` helpers start working the
moment `str-concat` is shimmed.

### Phase 1 -- pattern parser (cstr -> AST)

A hand recursive-descent parser over an `{cstr, pos}` input model (mirror
`parsec.tur:11-14`, reading bytes via `cstr-nth`), producing:

```turmeric
(defdata Re
  (RChar   :int)          ; a literal byte
  (RAny)                  ; .
  (RClass  :int :bool)    ; char-class handle + negated? (start simple: enumerated set)
  (RStar   :Re) (RPlus :Re) (ROpt :Re)
  (RConcat :Re :Re)
  (RAlt    :Re :Re)
  (RGroup  :Re)           ; capturing
  (RAnchorStart) (RAnchorEnd))
```

Start with the common subset (literals, `.`, `* + ?`, `|`, `(...)`, `^ $`, simple
`[...]`); add `{n,m}` and POSIX class names incrementally. `re/compile` returns
the parsed `Re` (as a `defopaque Regex`), replacing the `regex_t` pointer.

### Phase 2 -- backtracking matcher

`(match-here : Re -> Input -> int -> option<int>)` returning the end index on
success, threaded so `RStar`/`RPlus` try longest-first (leftmost-longest-ish) and
`RAlt` tries left then right. Then build the public API on top, all pure:

- `re/match?` -- try `match-here` at each start position (unanchored), true on
  first success.
- `re/match` -- return start/end (and, once groups are threaded, captures) as a
  `vec<cstr>` via `cstr` slicing built from `cstr-nth` + `str-concat`.
- `re/find-all` -- loop, advancing past each match (advance by 1 on empty match to
  avoid the stall the C guards at `re.tur:170`).
- `re/replace` / `re/replace-all` -- splice `pre + replacement + suffix` with
  `str-concat` (literal replacement, matching current no-backref behavior).

`re/free` / `re/match-free` / `re/find-all-free` become no-ops or GC-managed
(the AST and result lists are ordinary heap values now, not `malloc`'d C
buffers) -- keep them as no-op shims one release for source compatibility.

### Phase 3 -- captures (optional, second pass)

Thread group start/end indices through the matcher so `re/match` returns real
capture slices (index 0 = whole match, 1..n = groups), restoring parity with the
POSIX `re_nsub` behavior (`re.tur:97`). Ship Phase 2 without this if needed;
`re/match?` and `re/find-all` don't require it.

### Phase 4 -- drop the carve + prove browser support

1. Delete `tests/fixtures/reader-macros-rx-literal/requires.tur-only`; it now
   runs under `--interpret`.
2. Add interpreter-path regex fixtures (match/find/replace) covering the common
   syntax.
3. Browser proof: load `stdlib/re.tur` in the WASM REPL and evaluate
   `(re/match? #rx"^[0-9]+$" "123")` -- it works with no WASM build change (the
   embedded `/stdlib/re.tur` loads on demand into the shared interpreter).

---

## Missing language features

**None fundamental.** ADTs, recursion, closures, `option`/`result`, cons lists
are all present and interpreter-exercised. The only true gap is the Phase 0 byte
natives (`cstr-nth`/`cstr-len`) plus interpreter `str-concat` -- absent today,
trivial to add. Everything else is a pure library exercise.

Compatibility note: POSIX ERE is leftmost-*longest*; a backtracking port is
leftmost-*first*. For the anchored/simple patterns in the fixtures the results
coincide, but document the semantics change, and prefer the Thompson-NFA
follow-up if strict POSIX longest-match parity matters to a downstream user.

---

## Validation / definition of done

- `re/match?` / `re/match` / `re/find-all` / `re/replace` run under
  `tur --interpret` (single acceptance gate for browser support).
- `reader-macros-rx-literal` runs un-carved under `tests/run-turi.sh`.
- `bash tests/run.sh` green (the compiled path now uses the pure engine too;
  regen snapshots in-PR; verify no behavior regression on existing re fixtures).
- Manual browser check: `#rx"..."` matches in the WASM REPL.
- `re.tur` contains zero `\`\`\`c` blocks.

---

## See Also

- `stdlib/re.tur`, `stdlib/parsec.tur:11` (Input-model reference),
  `stdlib/str.tur`, `stdlib/cstr.tur`.
- `src/compiler/reader.c:2625-2644`, `reader_macros.c:31` (the `#rx` literal --
  already portable, do not touch).
- `src/web/wasm_glue.c:74-172`, `src/CMakeLists.txt:865` (WASM stdlib embedding +
  shared interpreter path -- the "free browser support" mechanism).
- `docs/archive/history/turi-interpret-flip-residual-plan.md` Bucket R6 (where the
  "disproportionate library work" carve was recorded -- this port lifts it).
