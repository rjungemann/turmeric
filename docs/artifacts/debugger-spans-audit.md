# Debugger Phase 1 -- Source-Spans Audit

Status: landed (audit + coverage gate)
Track: docs/archive/history/debugger-plan.md, Phase 1 ("suggested first PR")

This is the Phase 1 deliverable from [debugger-plan.md](./debugger-plan.md): an
inventory of how source spans flow through the compiler, plus a coverage gate
that keeps breakpoint-eligible AST nodes anchored to a real source location so
the later debugger phases have something to stop on.

## What a debugger needs from spans

Every node a programmer can set a breakpoint on, or that can appear as a stack
frame, must resolve back to a `{file, line, col}` location. The plan names four
such **breakpoint-eligible** categories:

- **top-level form** -- the entry points listed in a module / program body
- **defn** -- function definitions (`EX_FN_DEF`)
- **let form** -- `EX_LET` / `EX_LETREC`
- **call site** -- `EX_CALL`

The exit criterion: for every fixture under `tests/fixtures/`, each such node
carries a non-zero span that round-trips through elaboration.

## Current span coverage (inventory)

Spans are represented by `struct Span { file_id, line, col_start, col_end,
off_start, off_end }` (`src/compiler/forms.h:11`), with the all-zero sentinel
`SPAN_UNKNOWN` and the `span_is_unknown()` predicate.

| Stage | Carrier | Span? | Notes |
| --- | --- | --- | --- |
| Reader | `Form.span` | yes | assigned in `reader.c` (`span_from_to`) for every form |
| Elaboration | `Expr.span` | yes | copied from `Form.span` in `elab_*.c` |
| Bindings | `Binding.span` | yes | declaration site (`expr.h`) |
| Macro expansion | substituted `Form`s | yes (preserved) | call-site spans survive substitution |
| Diagnostics | error messages | yes | `diag.c` renders `file:line` straight from `Span` |
| LSP | symbol ranges | yes | `lsp.c` converts 1-based spans to LSP ranges |
| Interpreter values | `TuriValue` | **no** | runtime values carry no location (Phase 2 concern) |
| C emitter | generated C | **no `#line`** | spans exist on `Expr` but are not emitted (Phase 4) |

The takeaway: the Form -> Expr -> Binding pipeline already carries spans well
because diagnostics depend on them. The gaps are downstream of elaboration
(interpreter runtime values; `#line` in emitted C) and are addressed by later
phases, not Phase 1.

### Known holes the audit deliberately does *not* flag

These are real but out of Phase 1's scope; they live on synthetic nodes that
have no user-source origin, or on stages the audit does not inspect:

- **Synthetic elaboration nodes** -- e.g. the `__args` binding for variadic
  params (`elab_core.c`) and synthesized dependency-metadata forms (`pkg.c`)
  use `SPAN_UNKNOWN` on purpose. They are not breakpoint targets.
- **Transform-pass output** -- CPS / effect-lowering / borrow passes mint new
  `Expr` nodes that legitimately lack a source span. The audit runs **right
  after `PASS_ELABORATE`**, before these passes, so it never sees them.
- **Interpreter runtime values / emitted-C `#line`** -- Phase 2 and Phase 4.

## The `tur audit-spans` command

`tur audit-spans <file>` elaborates the file (auto-discovering the enclosing
spice `src/` and `-I` flags exactly like `tur check`), then walks the
post-elaboration program tree and reports each breakpoint-eligible node whose
span is `SPAN_UNKNOWN` or carries no line. It never emits code.

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | clean -- every breakpoint-eligible node has a usable span |
| 3 | holes found (one line printed per hole) |
| 1 | did not elaborate (error fixture, or `requires.spices` sibling absent) |
| 2 | input file error |

The walk lives in `src/compiler/span_audit.c`; its per-kind child traversal
mirrors the effect-row pass (`src/passes/effect_check.c`) so `Expr`-union
coverage stays in step with the rest of the compiler.

## The coverage gate

`tests/check-span-coverage.sh` runs `audit-spans` over every fixture and fails
on any hole (exit 3); fixtures that do not elaborate in the current environment
are skipped. It is registered as the `tur_span_coverage` ctest target.

Current result: **1434 fixtures clean, 0 holes**, 4 skipped (two TSan-only,
two `requires.spices` whose sibling checkout is absent). The Phase 1 exit
criterion holds across the by-value fixture suite.

## Next

- Phase 2 (interpreter debugger) can rely on every breakpoint-eligible node
  having a `{file, line}` from this audit.
- Phase 4 (`#line` in emitted C) is the next span consumer; when it lands, the
  emitter will need the same post-elaboration spans this audit verifies are
  present.
