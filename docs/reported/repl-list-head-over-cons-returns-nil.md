# REPL `list-head`/`list-tail` over a prompt-built `cons` returns `nil`

**Severity:** medium (silent wrong answer at the interactive prompt; the
`--interpret` and compiled paths are correct, so it is REPL-only).

## Summary

In the tree-walking REPL (`tur repl`), reading the head/tail of a cons cell
built at the prompt with `cons` yields `nil` instead of the stored element.
The same expression is correct under `tur interpret` and in compiled code, so
the carrier layout is fine end-to-end -- only the interpreter's prompt-level
`cons` / `list-head` native pairing disagrees.

## Minimal repro

```
$ tur repl
turmeric> (list-head (cons 65 (cons 66 0)))
=> nil                      ; WRONG -- expect 65
```

```
$ cat lh.tur
(defn main [] : int (println (list-head (cons 65 (cons 66 0)))) 0)
$ tur interpret lh.tur
65                          ; correct
```

Reproduces on the shipped v0.30.4 binary and on the current tree; independent
of `:reset` (occurs in a fresh session too).

## Root cause (not yet pinned to a line)

`cons`, `list-head`, and `list-tail` are all registered as interpreter natives
(`src/turi/interpreter_natives.c:3004-3005` for the accessors, `:3061` for
`cons`). The accessors read a `struct { int64_t head; int64_t tail; }` at
offset 0/8 (`stdlib/list.tur:354-365`). The prompt-built cell coming out of the
`cons` native evidently does not present that layout at the pointer
`list-head` receives (boxing/tag indirection, or `cons` building a typed
`Cons`-tag value the raw-carrier accessor then misreads). Because the compiled
and `--interpret` paths agree with the stdlib layout, the divergence is in the
interpreter's `native_cons` / `native_list_head` pairing, not in the codegen or
the stdlib source.

## Fix directions

- Compare `native_cons` against `native_list_head`/`native_list_tail` in
  `src/turi/interpreter_natives.c`: confirm both operate on the same
  representation (raw `__tur_cell_t*` carrier vs a boxed/tagged `Cons`). One
  side is almost certainly boxing while the other reads the raw carrier.
- Cross-check against how `tcons`/`car`/`cdr` behave at the prompt -- if
  `(car (cons 65 0))` also returns `nil` the fault is squarely in `native_cons`;
  if `car` is fine but `list-head` is not, the two accessors have drifted.
- Add a REPL fixture (`(list-head (cons 65 (cons 66 0)))` => `65`) once fixed;
  the current suite only exercises these via compiled/interpret fixtures, which
  is why the prompt-only divergence slipped through.
