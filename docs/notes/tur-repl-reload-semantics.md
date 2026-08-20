# `tur repl :reload` semantics -- recon memo (L1)

Context: repl-load-definitions-plan L1 (the plan document itself is not
checked into the repo).
Goal: nail down what `:reload <file>` actually does today so the rest of the
plan (cmd+r driving the running REPL) can be designed honestly.

> **Outcome:** shape A below shipped. The REPL now has `:run <file>`
> (`cmd_run` in `src/turi/repl.c`, which cites this memo) and
> `:load-string "<src>"` for evaluating a multi-line region without a
> scratch file. The `:reload` mechanics described in the findings are
> unchanged. Line references inside the findings are from the v0.25.x
> tree this memo was written against and have drifted; the Pointers
> section at the bottom carries current (v0.36.0) anchors.

## Findings

### 1. Same env, not a sub-env

`cmd_reload` (src/turi/repl.c:382) calls `turi_eval_file` (src/turi/eval.c:8357),
which slurps the file into a buffer and hands it straight to `turi_eval` on
the **same** `TuriEnv *` that the REPL loop owns. There is no sub-env, no
fresh arena root, no isolation.

### 2. Source accumulator is the load-bearing thing

`turi_eval` (src/turi/eval.c:8065 -> turi_eval_impl) does NOT incrementally
register new defns into an existing program tree. On every call it:

1. Builds `combined = env->src_acc + "\n" + new_src` (eval.c:8138-8145).
2. Re-parses the **entire combined buffer** as one program.
3. Re-elaborates from scratch (eval.c:8249).
4. Evaluates only the slice `[n_fsd + prior_toplevel .. total)` so prior
   forms aren't re-run, but they ARE re-parsed and re-elaborated each call
   (eval.c:8329).
5. On success, appends the new source to `env->src_acc` and advances
   `prior_toplevel` (eval.c:8336-8340).

Defn evaluation itself is idempotent: `EX_FN_DEF` and `EX_DEF` cases call
`turi_env_set` (eval.c:6515, 6445) which overwrites whatever was bound under
that name. So if elaboration succeeds, redefinition works.

### 3. Re-`:reload` of the same file BREAKS

Empirical (Turmeric v0.25.5):

```
:reload /tmp/reload_a.tur       # (defn hello [] : int 42)
(hello)                          # => 42
:reload /tmp/reload_b.tur       # (defn hello [] : int 99) (defn extra [] : int 7)
# error: defn: 'hello' is already defined by an auto-loaded stdlib module;
#   rename the local definition
# warning [TUR-W0040]: unknown name 'extra'; will runtime-dispatch
(extra)                          # => error: unknown function or operator 'extra'
```

Root cause: the source accumulator. After the first `:reload`, `src_acc`
contains `reload_a.tur`'s text. The second `:reload` appends `reload_b.tur`
to that, so re-elaboration sees `(defn hello ...)` twice in one program and
hard-errors at elaboration time. Because elaboration failed, neither the
new `hello` nor `extra` got registered in the env -- the user is stuck with
the old `hello` and no `extra`. The "auto-loaded stdlib module" wording is a
misattribution from the elaborator's duplicate-defn message; nothing
stdlib-related is involved.

### 4. Removed bindings would leak even if redefinition worked

Even if we suppressed the duplicate-defn error, `turi_env_set` only adds; it
never removes. So a name that was in run N but not run N+1 stays bound to
its run-N value. DrRacket's "drop stale bindings" behaviour is not free here.

### 5. Diagnostics: stderr

Parse / elaboration errors emit through `diag_*` to stderr. `turi_eval`
returns `turi_error("parse error")` / `turi_error("elaboration error")` to
the caller, and `cmd_reload` suppresses those specific strings to avoid
printing them on top of the diagnostic the diag layer already rendered
(repl.c:386-390). Other error messages go through `fprintf(stderr,
"reload error: %s\n", ...)`.

### 6. No `:eval` meta-command

(Since superseded: `:load-string "<src>"` now evaluates source handed over
directly, so a host can send a multi-line region without a scratch file.)

There is no "evaluate this string in the current env" meta-command. Plain
top-level input at the prompt IS that primitive (the REPL loop's normal
`turi_eval` path), but there is no machinery to scope-isolate or
sandbox-evaluate a snippet. L5's "send selection to REPL" can piggy-back on
the normal prompt path; no new compiler-side surface needed.

### 7. `:reset` exists and does what it says

`:reset` (repl.c:852-863) frees the env, allocates a fresh one, and
re-registers the `(reload)` native. `src_acc` starts empty, `prior_toplevel`
back to 0. This is the only "clean slate" tool today.

## Implication for L2

The plan's "issue `:reload <abs-path>` to the REPL stdin" cannot be the
implementation because re-pressing cmd+r will fail at elaboration. Two
viable shapes:

- **A. New `:run <file>` meta-command** that resets the env then loads the
  file, then optionally `(main)`s. Trade-off: every Run wipes interactive
  bindings made at the prompt, matching DrRacket's "Run resets Interactions"
  semantic. This is what the plan implicitly wants.
- **B. Make `:reload` smarter** -- track the byte range of a previously-
  loaded file inside `src_acc` and truncate it before re-appending. More
  surface area; same observable behaviour as A for the cmd+r case. Defer.

L2 takes shape A: add `:run <file>` to `src/turi/repl.c`, and push
`:run <abs-path>` from any client (the `(main)` call is auto-invoked by
`:run`, so no client-side probe is needed). This shipped -- see `cmd_run`
in `src/turi/repl.c`, which resets the env, reloads the file, and
auto-invokes `(main)` when it resolves to a closure.

L3 (diagnostic surfacing) and L4 (stale chip) are unblocked by A. L5
(send-form-at-cursor) just sends the form text to the existing prompt path,
unchanged.

## Pointers (as of v0.36.0)

- `src/turi/repl.c:850` -- `cmd_reload`
- `src/turi/repl.c:957` -- `cmd_run` (`:run <file>`, shipped from this memo's shape A)
- `src/turi/repl.c:1494` -- `:reset` handler
- `src/turi/eval.c:11780` -- `turi_eval_file`
- `src/turi/eval.c:10575` -- `turi_eval` entry
- `src/turi/eval.c:11430-11431` -- source accumulator concatenation
- `src/turi/eval.c:11713-11714` -- post-success accumulator update
- `src/turi/eval.c:8868-8945` -- def / defn evaluation (`turi_env_set`)
