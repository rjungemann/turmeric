# Macro System Direction -- procedural macros on turi, not phases

Status: direction adopted; Stage 0 landed; Stages 1+ unscheduled.

## The question

The macro-time language is a small, closed compile-time (CT) evaluator
inside elaboration (`src/compiler/elab_macros.c`): quasiquote plus
`if`/`do`/`let`/`letrec`/`fn`/`map` and ~18 builtins, no arithmetic, no
calling runtime functions.  To expand macro power, do we (a) keep growing
this bespoke macro-time language, (b) adopt Racket-style phases, or (c)
something else?

## The answer: (c), a hybrid

**Freeze the CT language where it stands; keep template `defmacro` as the
permanent fast path; add an explicitly opted-in procedural macro form
whose body is ordinary Turmeric evaluated at expansion time by turi.**

Why not (a): every capability added to the CT evaluator (arithmetic,
strings, type inspection) is a second, drifting implementation of the
language, with its own bug trail -- see the ~15 archived macro-gap reports
in `docs/archive/history/` -- while a near-complete interpreter of the
real language (`src/turi/eval.c`) ships in the same binary, uncalled by
the compiler.

Why not (b): Racket's phase tower exists to make separate compilation,
per-phase module instantiation, and hygiene sound.  Turmeric elaborates
whole-program from source in one pass, macros are define-before-use, and
hygiene is deliberately manual -- the problems phases solve do not arise
here.  The one idea worth borrowing is small: an explicit annotation for
"this import is needed at expansion time" (Stage 3).

Why (c) fits unusually well: Clojure has no phases because the compiler
runs inside the runtime -- each top-level form is compiled and loaded
sequentially, so macros are just fns called with forms as data.  Turmeric
cannot do that literally (AOT to C), but turi is the next-best thing:
full-language, in-process (`TURI_EVAL_SOURCES` is linked into `tur`), and
sharing the compiler's exact reader and `elaborate_program_session`.
Clojure's "everything above is loaded" invariant maps exactly onto the
existing define-before-use rule, and everything downstream of
`expand -> Form*` (provenance notes, depth guards, definstance
re-spanning -- `elab_call.c`) is reused unchanged.  The one genuinely new
piece is a syntax value crossing the boundary.

## CT language freeze -- POLICY

The compile-time evaluator is **frozen**: no new builtins in
`CT_BUILTIN_TABLE`, no new `CtValue` cases (explicitly: no `Type` case,
no integers), no arithmetic.  The depth-cap diagnostic's "the compile-time
evaluator has no arithmetic" note stays true.  New expressiveness demand
routes to Stage 2's procedural macros, where the answer is "the whole
language" instead of one more bespoke builtin.  (Type-level reflection
remains governed by R3 of
[row-types-followups-plan.md](hold/row-types-followups-plan.md): bounded,
total accessors if ever, never a general evaluator.)

## Stage 0 -- quick wins (LANDED)

- `CT_BUILTIN_TABLE` X-macro in `elab_macros.c`: one table drives both
  `ct_eval_builtin` dispatch and the template-routing gate
  (`template_needs_ct_eval`), killing the historical "builtin added to
  the evaluator but not the gate, silently never fires" bug class.
- Unified `gensym`: one implementation (`gensym_fresh`) for all three
  call paths, with freshness checked against the symbol table
  (`symtab_contains`) so user-written `tmp_0` cannot be captured; a
  `(gensym)` reaching ordinary runtime code is now a plain error instead
  of a half-baked TYPE_INT binding.
- `tur expand <file>`: prints every macro expansion to stdout with
  `;; name @ file:line:col` headers; diagnostics stay on stderr.  The
  intended golden-file mechanism for expansion tests.
- Multi-form `defmacro` bodies: setup forms + final template, wrapped in
  a synthetic CT `do`, always routed through the CT evaluator.

## Stage 1 -- syntax value + macro-time env plumbing (unscheduled)

- `TURI_SYNTAX` tag in `TuriValue` (`src/turi/value.h`) wrapping `Form*`
  directly (spans ride along); `TY_SYNTAX` opaque type in
  `src/compiler/types.h` (pattern: `TYPE_SYM`).
- ~25 syntax builtins as interpreter natives: accessors
  (`syntax-first/rest/tag/...`), converters (`syntax->int`,
  `int->syntax`, ... -- what permanently fixes "no arithmetic at macro
  time"), span-inheriting constructors, `gensym`, and `syntax-error`
  (diag at the argument's span).
- **Stage gate: `g_interpret_mode` + globals reentrancy audit.**
  Macro-time eval nests inside a compile-mode elaboration turn.
  Templates exist (the JIT REPL's save/clear/restore in `main.c`; turi's
  per-env snapshot), but the three interp-flipped elaboration behaviors
  (`elab_toplevel.c` reader-conds, `elab_call.c` unknown-head demotion,
  `elab_core.c` aggregate returns), the diag sink, and
  `g_opt_cps_tramp_resume` all need a deliberate decision.
- Macro env: lazily created per-compile, torn down with the
  `ElabSession` (fresh compile-time state each build = determinism),
  `TURI_CAP_NONE`, fuel-limited alongside the existing depth-256 +
  stack-headroom guards.

## Stage 2 -- `defmacro*` procedural macros, same-module (unscheduled)

- New special form next to `defmacro` in `elab_call.c`; `MacroDef` gains
  `kind = TEMPLATE | PROCEDURAL` plus a turi closure.  At definition the
  body is elaborated as `(Fn [Syntax...] Syntax)` in the macro env
  (params implicitly `Syntax` -- zero annotation burden, definition-time
  type errors).
- Call path: convert args -> invoke closure (fuel + interp-mode
  bracketed) -> unwrap `Form*` -> feed the UNCHANGED downstream
  (provenance note, definstance re-span, toplevel hand-off, refine
  crossing note).  Interpreter error -> diag at the call site plus the
  existing "in expansion of" note.
- Routing is explicit, never heuristic: body size / feature sniffing
  would be the old gate bug at 100x blast radius.
- Proof-of-migration only: port the derive-show/debug/display family
  (`stdlib/macros.tur`), diffing `tur emit-c` output against the
  template versions; the other ~90 template macros stay as they are,
  permanently first-class.

## Stage 3 -- cross-module macro-time deps (unscheduled, post-v1 unless
a concrete stdlib need appears)

- Explicit `(import m :for-macros)` -- evaluates m's defns into the macro
  env (transient `TURI_CAP_IMPORT`); `exported_macros` carries the
  PROCEDURAL kind.  Explicit, never implicit: implicit would mean any
  imported module's code may run at compile time (determinism and
  supply-chain surface).
- `--macro-caps=io` for the rare legitimately effectful macro
  (embed-file style); default deny; never FFI/Unsafe/inline-C at macro
  time.

## Stage 4 -- convergence (post-v1, demand order)

- Reader-macro RM5 (function expanders) becomes a small feature over
  `Syntax`.
- Bounded type reflection per R3: total, structurally-recursive
  accessors as natives -- never a general `Type` value.
- JIT fast path (`tur_jit_compile_image`) as a transparent cache for hot
  procedural macros -- only after a syntax calling convention exists (the
  current FFI export ABI is scalars-only).

## What NOT to do

1. No Racket phase tower.
2. No hygiene overhaul bundled in -- unhygienic-with-gensym is documented
   design; changing it is an independent, breaking, post-v1 decision.
3. No heuristic routing between the two evaluators.
4. No JIT-first procedural macros.
5. No mass migration of the template macros.
6. No further CT-evaluator growth -- freeze means freeze.
7. No implicit macro-time imports.
