# LSP completion is dominated by compiler-internal names

**Severity:** medium (quality of the completion surface; no incorrect answers)
**Found:** 2026-07-29, while executing `docs/archive/try-turmeric-lsp-plan.md`
**Affects:** `tur lsp` and the Try Turmeric playground equally -- both read the
same symbol index.

**Status: RESOLVED 2026-08-05** via the report's second fix direction -- a
`Binding.is_synthesized` bit, not the `__` prefix test. The prefix premise did
not survive measurement; see the resolution note at the bottom of this file.

## Summary

An unfiltered `textDocument/completion` returns almost nothing a human typed.
The stdlib's typeclass instances elaborate into mangled global bindings
(`__inst_Eq_eq_qu_int`, `__inst_MapKey_mk_hybox_float32`,
`__inst_Monad_bind_Option`, ...) and anonymous lambdas lifted during
elaboration become `__fn_774`-style globals. `lsp_collect_program` records
every global binding, so all of them land in the index and then in the
completion response.

They also consume the response cap. `LSP_COMPLETION_MAX` is 200
(`src/lsp/lsp.c`), and on a trivial buffer the `__inst_*` names alone overrun
it -- so `isIncomplete` comes back `true` and the genuinely useful stdlib names
(`vec-new`, `cons`, `map-get`) are the ones cut.

## Repro

```sh
cat > /tmp/probe.tur <<'EOF'
(defn twice [x : int] : int (* x 2))
EOF
# drive `tur lsp`: initialize -> didOpen -> completion at line 1, character 0
```

Or, without a client, run the in-tree harness:

```sh
cmake --build build -j --target tur_lsp_wasm_backend_unit
./build/tur_lsp_wasm_backend_unit stdlib
```

and print the response in `test_stdlib_is_autoloaded` -- items 2 through ~200
are all `__inst_*`.

The two document-local-first passes in `on_completion`
(`src/lsp/lsp.c`, "Symbol completions, document-local first") mean the user's
own definitions are safe. Everything else is not.

## Root cause

`collect_binding` in `src/lsp/lsp_collect.c` filters on `b->is_global` and
nothing else. `is_global` is true for elaborator-synthesised bindings just as
it is for user-written ones; there is no "this name was written by a person"
bit on `Binding` to test.

## Fix directions

Cheapest, and probably right: skip names beginning with `__` in
`collect_binding`. The prefix is already the codebase's convention for
compiler-internal globals (`__inst_`, `__fn_`, `__functor_`), and a Turmeric
user has no reason to define one. This is one condition and it removes the
whole class.

Better, if `Binding` is being touched anyway: set a `synthesized` flag where
elaboration mints a binding with no user-written source form, and filter on
that. It survives a future naming convention change; the prefix test does not.

Note the prefix test must go in the collector, not in `on_completion` --
hover, go-to-definition, `documentSymbol`, and `workspace/symbol` read the same
index and have the same problem. `documentSymbol` on a file with a
`definstance` currently lists the mangled instance methods in the editor's
outline view.

## Resolution (2026-08-05)

`Binding` gained an `is_synthesized` bit, set at the two elaborator sites that
mint a name no person typed -- the lifted-lambda name in `elab_fns.c`
(`__fn_%u`) and the instance-method name in `elab_typeclasses.c`
(`__inst_<class>_<method>_<ty>`). `collect_binding` in `src/lsp/lsp_collect.c`
skips those bindings, which is the collector placement this report asked for
and is right for this class: there is no source form to navigate to and no
prefix a user would type, so no reader is worse off.

### Why the prefix test was not taken

The report's cheaper direction reads: *"skip names beginning with `__` ... the
prefix is already the codebase's convention for compiler-internal globals, and
a Turmeric user has no reason to define one."* The first half is true, the
inference from it is not. `__` in this codebase means **internal**, not
**synthesized**, and the stdlib uses it for its own hand-written helpers --
`__arrow_pair_first`, `__identity_extract`, `__pair_extend`, and ~46 others
across `arrow.tur`, `comonad.tur`, `sized-buf.tur`, `select.tur`, `thread.tur`,
`taskgroup.tur`, and more. Those have real source, real spans, and belong in
their own file's outline and under the cursor on hover. A prefix test in the
collector would have taken all of that away to remove three of them from one
completion response.

The distinction is not hypothetical: three such names (`__cons-fmap`,
`__tur-q-ok-val`, `__tur-q-is-err?`) are in the autoloaded set today and are
deliberately still offered. `tests/lsp/wasm_backend_test.c` pins that with a
buffer-local `__buffer-helper` rather than a stdlib name, so the assertion is
about the rule rather than about a name that could be renamed later.

A dedicated bit rather than `is_instance_method || is_lifted_lambda` (both of
which already existed and are exactly co-extensive with it today): those two
are specific facts the emitter and the `source_binding` alias rule consult, and
a consumer asking "did a person write this?" should not have to enumerate every
species of synthesized binding. A future mint site opts in with one assignment
instead of silently reintroducing the defect.

### Measured, before and after

The report's "items 2 through ~200 are all `__inst_*`" was an overstatement.
Measured on the same repro (an empty prefix against the autoloaded stdlib), the
response held **68 mangled names among 200 items** -- interleaved with real
ones, not a solid block. The substance held regardless: a third of a bounded
response was noise.

Full index for that scenario, from an instrumented `collect_binding`:

| | before | after |
| --- | ---: | ---: |
| `__inst_*` | 64 | 0 |
| `__fn_*` | 6 | 0 |
| hand-written `__*` | 3 | 3 |
| everything else | 360 | 360 |
| **total collected** | **433** | **363** |

Within the 200-item response that is 132 useful names before, 200 after.

### What this does not fix

**The cap is still reached.** 363 collected against `LSP_COMPLETION_MAX` of 200
means an empty-prefix query still comes back `isIncomplete: true` and still
drops real stdlib names. This report was about *what* filled the response, and
that is fixed; the size of the response is a separate question. It is also a
much weaker one -- `isIncomplete` is the protocol's signal for "re-query as the
prefix narrows", and with any prefix typed the truncation is rare. Raising the
cap or ranking within it should be its own change with its own reasoning, not a
side effect of this one.
