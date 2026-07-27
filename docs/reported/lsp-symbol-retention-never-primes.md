# LSP: last-good symbol retention never primes for a file that has not yet parsed

**Severity:** medium (completion silently dead in a real, common scenario --
opening a file that already has a syntax error).

Reported from the consumer side by the agent working on
[Trowel](https://github.com/rjungemann/trowel), the native Turmeric editor,
against `claude/busy-clarke-6zj9jl` (HEAD `8f341d32e`). Verified here.

## Summary

The §2.1 fix in [docs/upcoming/lsp-client-gaps-plan.md](../upcoming/lsp-client-gaps-plan.md)
retains the previous symbol index when a compile yields nothing, so completion
survives the unbalanced-paren state that is normal while typing. It works --
**but only once a successful analysis has run at least once for that document.**

If the buffer has never parsed, there is no last-good set to fall back on and
completion returns an empty list, exactly as it did before the fix. The
realistic version is a user opening a file that already contains a syntax
error: completion stays dead until they fix the error unaided, which is when
they most want it.

## Repro

Drive `tur lsp` over stdio. The two runs differ in exactly one thing --
whether any request forced an analysis before the buffer was broken.

```
GOOD   = '(defn zork [a :int] :int a)\n'
BROKEN = GOOD + '\n(smoke\n'

A)  didOpen(GOOD) -> documentSymbol -> didChange(BROKEN) -> completion  => 200 items
B)  didOpen(GOOD) ->                   didChange(BROKEN) -> completion  =>   0 items
```

And the case that motivated the report -- a file that is broken on disk:

```
C)  didOpen('(defn zork [a :int] :int a)\n(smoke\n') -> completion       =>   0 items
```

(B) matters because analysis is *debounced*: `didOpen` only marks the document
dirty, so a client that opens a file and edits it before the first flush lands
in the unprimed state without doing anything unusual.

## Root cause

`src/lsp/lsp.c`, `run_doc_analysis`, the adoption condition:

```c
if (rc == 0 || fresh_count > 0 || !doc->symbols) {
    /* adopt `fresh` */
} else {
    free(fresh);
    doc->symbols_stale = 1;
}
```

The `!doc->symbols` disjunct is what breaks priming. On the first analysis
`doc->symbols` is NULL, so the empty result is adopted unconditionally --
including when the compile failed. From then on `doc->symbols` is non-NULL but
*empty*, and every later failure takes the retain branch and faithfully retains
the empty set.

That disjunct is not gratuitous: it exists so a file legitimately emptied of
definitions reports empty rather than staying stale forever. The bug is that it
cannot distinguish "successfully analyzed, genuinely has no symbols" from
"never successfully analyzed".

## Why the existing test did not catch it

`test_lsp_client_gaps` in `tests/lsp/mcp_lsp_test.py` issues a
`textDocument/completion` between the `didOpen` and the broken `didChange`.
That request forces `lsp_flush_dirty`, which primes the index -- so the test
exercises path (A) and passes for a reason that hides (B) and (C). Any fix
should add an unprimed case rather than trusting the current one.

## Fix directions

The adoption condition needs to track *whether a good analysis has ever
happened*, not whether a pointer is NULL. Add e.g. `int ever_analyzed` to
`LspDoc` (`src/lsp/lsp_docs.h`), set it on the first `rc == 0`, and adopt an
empty result only when it is set.

That alone fixes (B) but not (C) -- a file broken from the start still has
nothing of its own to retain. For (C) the useful fallback is the stdlib
surface, which is document-independent: every successful analysis of *any*
document collects the same ~200 stdlib symbols, so caching them process-wide
on the first success and serving them when a document has no index of its own
would give a user opening a broken file the stdlib completions at least. Worth
weighing against the risk of offering symbols that the broken file would not
actually have in scope.

## Not to be confused with

The retention fix itself, which works. The consumer explicitly confirmed
`(smoke-` prefix completion in a broken buffer returns correct filtered results
once primed, and called it the most valuable item on the list. This report is
only about the unprimed edge.
