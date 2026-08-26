# Fix: statement position no longer deletes wrapped expressions

Resolves
[poly-call-in-statement-position-dropped](../poly-call-in-statement-position-dropped.md).

## Root cause

`emit_stmt` (`src/compiler/emit_stmt.c`) keeps a list of node kinds it treats
as "pure -- emit nothing" in statement position. Four of those kinds are pure
**wrappers** around an inner expression:

    EX_REINTERPRET   EX_CAST   EX_ASCRIBE   EX_POLY_WRAP

The wrapper is pure; the expression inside it may not be. Emitting nothing
deleted the wrapped expression, effects and all.

The report's signature asymmetry -- parametric calls dropped at every
instantiation *except* `int` -- falls straight out of this. A discarded
parametric call's result rides the int64 carrier, and elaboration wraps the
call to restore the instantiation type:

    (do (reinterpret int -> bool <call>) <call> 0)
                                          ^^^ the @int call needs no wrapper

The `@bool` / `@float` / `@cstr` calls arrive at `emit_stmt` as
`EX_REINTERPRET` and hit the emit-nothing arm; the `@int` call arrives as a
bare `EX_CALL` and is emitted. Nothing about the drop was specific to
parametric functions -- a discarded `(:: (call) :T)` ascription or a cast
around a call was the same hole through a different door.

Diagnosis note: the tree-walking interpreter runs the natural spelling
correctly, which localized the bug to emit -- but only weakly, since turi
walks the form tree rather than the elaborated Expr tree. What settled it was
dumping the elaborated body `emit_fn_def` receives (a temporary probe,
removed): the call was present, wrapped, and then not emitted.

## The fix

The four wrapper kinds now **delegate to their inner expression's statement
form** instead of emitting nothing:

    case EX_REINTERPRET: emit_stmt(ctx, body, e->as.reinterpret_.expr); return;
    case EX_CAST:        emit_stmt(ctx, body, e->as.cast_.expr);        return;
    case EX_ASCRIBE:     emit_stmt(ctx, body, e->as.ascribe_.inner);    return;
    case EX_POLY_WRAP:   emit_stmt(ctx, body, e->as.poly_wrap_.inner);  return;

Delegating (rather than routing through `emit_value` and discarding) keeps the
inner expression's own statement-position handling -- e.g. a wrapped
catch-unwind still gets its result-box free. The pure wrapper itself vanishes,
which was the only part of the old behavior that was right.

Kinds deliberately left in the emit-nothing group: true leaves (literals,
vars) and value-constructors whose operands elaboration only feeds simple
values in practice (`EX_UNION_INJECT`, `EX_EXISTS_PACK`, `EX_ANY_*`,
`EX_CONS_LIST`). If one of those is ever found wrapping an effectful call in
statement position it is this same bug and this same one-line fix shape.

## Verification

- `poly-statement-position-effect` -- the fixture that was red on purpose --
  is green: all four instantiations print `  ran`.
- Full suite: **2698 passed, 0 failed.** Zero `expected.c` snapshot drift --
  no existing snapshot contained a wrapped statement-position call, which is
  also why the suite never caught this.
- All three workaround sites from the report reverted, per its protocol:
  - `stdlib/trail.tur`: HAZARD block deleted, `with-untrailed`'s example
    restored to the natural discarded form.
  - `tests/fixtures/sx2-trail-combinators`: discards the result -- the
    spelling users write -- and prints 55 only if the discarded write actually
    happens, so it doubles as the regression pin for stdlib reach.
  - No further `with-*` brackets had accumulated behind it.
- Proof ran as the report specified: the discarded-form `with-untrailed` write
  happens (55, not the rolled-back 0).
