# Turmeric Debugger: In-Frame Expression Evaluator

Status: sketch / proposed
Owner: unassigned
Track: post-v1 polish. Cross-cutting enhancement to debugger Phases 2-3
(interpreter debugger + DAP); not a sequential phase of the parent plan.
Parent: [debugger-plan.md](./debugger-plan.md)

## Goal

Evaluate an **arbitrary Turmeric expression in the lexical scope of a paused
debugger frame**. Today the debugger can resolve a *single name* against a
paused frame (`print <name>` in the text REPL; `evaluate`/hover and
single-name conditions in DAP), but it cannot evaluate `(+ a 1)`,
`(vec-get xs 0)`, or `(> (count items) 3)`. That one capability unlocks the
two features both shipped debugger phases explicitly deferred:

1. **Full-expression conditional breakpoints** -- `break <line> if <expr>` in
   the text REPL and an arbitrary `condition` string in DAP `setBreakpoints`,
   replacing the simple `<name> <op> <literal>` shim in `src/turi/dap.c`
   (`dap_eval_condition`).
2. **Arbitrary `evaluate`** -- the DAP `evaluate` request (Debug Console / hover)
   and a text-REPL `eval <expr>` command, beyond single-binding lookup.

## Where the two phases left off

- Phase 2 (`debugger-interpreter-phase2.md`): "evaluating an arbitrary
  expression in the paused frame's lexical scope is deferred. `print <name>`
  covers single-binding inspection in the meantime." `dbg_add_breakpoint` parses
  only `<file>:<line>` -- there is no `if <expr>` arm yet.
- Phase 3 (`debugger-dap-phase3.md`): `evaluate` resolves a single name
  (`turi_debug_eval_name`); conditional breakpoints accept only a single
  comparison `<name> <op> <literal>` (`dap_eval_condition`), with an unparseable
  or unresolved condition degrading to "stop" so a breakpoint is never silently
  swallowed.

Both are intentionally narrow shims that this plan generalizes.

## The shape of the problem

The interpreter already has a full parse -> elaborate -> eval pipeline
(`turi_eval(env, src)` in `src/turi/eval.c`), but it runs against the env's
**top-level** scope. The debugger holds a paused **runtime** frame
(`dbg->cur_frame`, an `EvalFrame` chain of `EvalBinding {name, value}`), and the
gap between them has three parts:

1. **Parse** the expression string -- trivial; reuse the existing reader.
2. **Elaborate** it -- the hard part. Elaboration is type-directed, so to
   type-check `(+ a 1)` the elaborator needs `a`'s **type**. But a runtime
   `EvalBinding` carries a `TuriValue` (a dynamically tagged value), not an
   elaborated `Type`. The locals' static types are gone by the time we pause.
3. **Eval** the elaborated node against an `EvalFrame` that has the locals bound
   -- straightforward once 1-2 produce a node; `dbg->cur_frame` is already the
   right frame.

So the whole plan is really about **bridging step 2**: recovering enough type
information about the paused frame's locals to elaborate a fresh expression
against them.

## Approach (recommended): runtime-tag type reconstruction

Synthesize a typed elaboration scope from the paused frame by mapping each
in-scope binding's `TuriValue` tag to a Turmeric type, then elaborate the
expression with that scope pushed as the innermost lexical layer:

| `TuriTag` | Recovered type |
|---|---|
| `TURI_BOOL` | `:bool` |
| `TURI_INT` | `:int` |
| `TURI_FLOAT` | `:float` |
| `TURI_STRUCT` | the instance's struct type (`TuriStruct` carries its descriptor) |
| `TURI_STRUCT_TYPE` | the type descriptor itself |
| `TURI_CLOSURE` | the closure's declared `(fn [..] ..)` type, if retained |
| `TURI_NIL` | `:nil` (or a fresh tyvar -- see open questions) |

This is lossy for type-erased values -- a `defopaque Route :int` reads back as
`:int`, and a `cons`/`vec`/`option`/`result` flowing as the `:int` carrier reads
back as `:int` rather than its rich type. That is acceptable for a debug-console
evaluator: the expression still *runs* (the interpreter is dynamically tagged),
and the worst case is a `tur check`-style type error on an expression the user
could rephrase. Where elaboration would reject a value whose static type we
could not recover, fall back to a **lenient elaboration mode** that admits the
binding at a fresh type and defers to runtime dispatch (the interpreter already
keeps a runtime-native dispatch fallback for unbound call heads, "UCH1"), rather
than failing the evaluate outright.

### New API surface (eval.h)

```c
/* Evaluate `src` as a Turmeric expression in the lexical scope of paused frame
 * `idx` (0 = innermost). Renders the result into out_repr. Returns false (with
 * a short message in out_repr) on a parse / elaboration / runtime error -- never
 * aborts the debuggee. Nested debugger stops are suppressed for the duration
 * (the existing dbg->in_repl guard already does this). */
bool turi_debug_eval_expr(TuriEnv *env, int idx, const char *src,
                          char *out_repr, size_t cap);
```

`turi_debug_eval_name` becomes a thin special case of this (or stays as the fast
path for the common single-name lookup).

### Consumers

- `src/turi/dap.c`: `dap_evaluate` calls `turi_debug_eval_expr` instead of
  `turi_debug_eval_name`; `dap_eval_condition` is deleted and the conditional
  hook (`dap_on_cond`) calls `turi_debug_eval_expr` and tests the result for
  truthiness (`turi_is_truthy`).
- `src/turi/eval.c` (text REPL): add `eval <expr>` and parse a `break <line> if
  <expr>` arm in `dbg_add_breakpoint`; store the condition in the existing
  `DbgBreakpoint.cond` slot (already present from Phase 3) and run it through the
  same `cond_fn`.

## Alternative considered: a restricted mini-evaluator

Walk the parsed s-expression directly against the `EvalFrame` plus a whitelist
of operators / stdlib calls, skipping elaboration entirely. Simpler and
type-error-free, but it reimplements a second evaluator, drifts from real
language semantics, and caps what conditions can express. Rejected as the
primary path; may be a useful *fallback* for the lenient mode above.

## Phasing

- **Phase A -- evaluator core.** Runtime-value -> type reconstruction, the
  elaborate-with-locals scaffolding, `turi_debug_eval_expr`, and the lenient
  fallback. Wire into DAP `evaluate` (arbitrary expressions) and a text-REPL
  `eval <expr>` command. Reentrancy + error containment covered by tests.
- **Phase B -- full conditional breakpoints.** Replace `dap_eval_condition`
  with `turi_debug_eval_expr` truthiness; add the `break <line> if <expr>` arm
  to the text REPL. Delete the simple-comparison shim.

## Out of scope

- **Expandable structured `variables`** (struct/option/result/vec/map children
  with non-zero `variablesReference`). Adjacent UX, but it is about the DAP
  variable-tree protocol, not expression evaluation -- track separately.
- **Setting** a value (`setVariable`) -- mutation, a different feature.
- Native (`emit-C`) expression evaluation under gdb/lldb -- Phases 4-5 territory.

## Exit criteria

- A DAP `evaluate` of `(+ a b)` in a paused frame returns the sum; an
  expression naming an out-of-scope binding returns a clean error (no abort).
- A conditional breakpoint `break <line> if (> i 3)` (text) and the equivalent
  DAP `condition` fire only when the predicate holds, with the same fixture the
  Phase 3 `tur_dap` regression uses (generalized past `i == 3`).
- Evaluating an expression at a stop never triggers a nested stop and never
  corrupts the paused program's state for the resumed run.

## Risks / open questions

- **Type recovery is lossy** (opaque newtypes, carrier-boxed
  cons/vec/option/result, generics). How aggressively should the lenient mode
  paper over a missing static type before reporting an "unevaluable here" error?
- **Side effects in conditions.** A condition expression may call a function
  with effects, allocate, or loop. Do we run conditions verbatim (document the
  footgun), restrict to an effect-free subset, or fuel-bound them like the
  sandbox (`TURI_DEFAULT_SANDBOX_FUEL`)?
- **`TURI_NIL` typing.** A `nil`-valued local has no useful recovered type;
  admit it at a fresh tyvar, or reject expressions that depend on it?
- **Elaboration cost per stop.** Re-elaborating a condition on every line-entry
  hit could be slow on a hot breakpoint; cache the elaborated condition node
  keyed on `(breakpoint, source)` so it elaborates once.
