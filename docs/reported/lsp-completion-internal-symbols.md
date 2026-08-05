# LSP completion is dominated by compiler-internal names

**Severity:** medium (quality of the completion surface; no incorrect answers)
**Found:** 2026-07-29, while executing `docs/upcoming/try-turmeric-lsp-plan.md`
**Affects:** `tur lsp` and the Try Turmeric playground equally -- both read the
same symbol index.

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
