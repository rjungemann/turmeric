# match arms cannot nest constructor patterns

**Severity: medium** (expressiveness) -- `(match e (Add (Lit 0) r) r ...)`
fails to elaborate; every fixture and guide flattens with an inner `match`.
Found in the 2026-08-20 docs audit.

## Repro

```turmeric
(match e
  (Add (Lit 0) r) r
  ...)
;; fails to elaborate; the workaround is
(match e
  (Add l r) (match l (Lit 0) r ...))
;; see tests/fixtures/adt-nested/input.tur, which uses exactly this shape
```

## Root cause

The match elaboration in src/compiler/elab_structs.c binds one constructor
level per arm; sub-patterns in field positions are treated as binders only.

## Fix direction

Recursive pattern compilation (decision-tree or nested-if lowering) with
exhaustiveness extended to nested depth.

## Guides to update when fixed

- docs/guides/gadts-guide.md ("No nested patterns in GADT arms" limitation)
- docs/guides/gadts-cookbook.md
- any other pattern-matching material that teaches the flattening workaround

## Resolution (2026-08-21)

Nested constructor patterns work, to any depth, with scalar literals and
when-guards, on `defdata` and `defgadt` alike. The report's own repro
(`(Add (Lit 0) r)` before `(Add l r)`) compiles, runs, and gives the same
answers under `tur run` and `--interpret`.

**Not the decision-tree rewrite the fix direction proposed.** The lowering is a
FORM-level rewrite in front of `elab_match`, so the arm loop, the exhaustiveness
check, the linear/borrow machinery and codegen are all untouched. All the arms
for one constructor fold into a single arm binding fresh field names, whose body
is a chain of tests over the nested sub-patterns falling through to the next
candidate on failure:

```
(match e                        (let [__ms_0 e]
  (Lit v)         v      =>       (match __ms_0
  (Add (Lit 0) r) r                 (Lit v) v
  (Add l r)       (f l r))          (Add __mp_1 __mp_2)
                                      (match __mp_1
                                        (Lit __mp_3)
                                          (if (= __mp_3 0)
                                            (let [r __mp_2] r)
                                            (let [l __mp_1 r __mp_2] (f l r)))
                                        _ (let [l __mp_1 r __mp_2] (f l r)))))
```

Depth needs no special handling: the inner `match` forms the lowering emits go
back through `elab_match` and are lowered again. The fallback is duplicated per
failing test -- code size, never execution. `TUR_DUMP_MATCH_LOWER=1` prints the
rewritten form.

Exhaustiveness is checked BEFORE the rewrite hides it, and is the one place the
lowering can refuse: a nesting group must either end in an arm binding plain
names, provably cover the sub-ADT (recursively, one nesting position per
group), or be followed by a catch-all. Otherwise it is an error naming the
constructor, not a silent runtime panic. When the group IS provably exhaustive
the unreachable fallback is a `(panic ...)`, which is why the two prerequisites
below were needed.

### Two prerequisite defects, each real on its own

Both reproduce without any nested pattern, both are fixed here, both are pinned
by `tests/fixtures/match-arm-diverging-and-move/`:

1. **A `!`-typed arm was rejected.** `(match e (Lit v) v (Add l r) (panic "no"))`
   failed with "match: arm types are incompatible -- expected int, got !", so a
   `match` could not have a panicking arm at all. `match_arm_type_compatible`
   now treats `TY_NEVER` as bottom, and the five arm-emission sites emit a
   never-typed body as a STATEMENT rather than assigning its `((void)0)` to the
   result temp (which the C compiler rejected with "void value not ignored").
2. **Match arms did not rewind move state.** Arms are alternatives, exactly like
   the branches of an `if` -- which has always snapshotted, rewound and merged
   `is_moved` (`before || (then && else)`). `match` rewound only the LINEAR
   state, so `(match t (TA) (f x) (TB) (f x))` was a spurious TUR-E0005
   use-after-move while the identical if/else compiled. Same snapshot / rewind /
   merge now, with diverging arms excluded from the vote since they cannot reach
   the merge.

### Known limitation, filed separately

A variable (non-`_`) catch-all arm on an ADT match still does not bind its
variable -- `(match o (OA n) n whole (g whole))` is "unbound symbol 'whole'".
That is pre-existing and independent (the ADT arm path records `pat->var_sym`
but creates no Binding, and the ADT emitter treats `is_var` as a wildcard); the
lowering emits the same alias a user would write, so such a fallback reports the
same diagnostic it does today. Filed as
[match-adt-var-arm-does-not-bind](../reported/match-adt-var-arm-does-not-bind.md).

Verified: `bash tests/run.sh` 2680 passed / 0 failed (no snapshot churn -- the
emitter change only fires for arms that previously could not compile) and
`bash tests/run-turi.sh` 1846 passed / 0 failed.

## Guides updated

- `docs/guides/gadts-guide.md` -- the "No nested patterns in GADT arms"
  limitation is deleted and replaced by a "Nested patterns" section (verified
  against a real `defgadt`).
- `docs/guides/sum-types-guide.md` -- the sub-match bullet now teaches direct
  nesting alongside the inner-match form. While verifying those samples, every
  sweet-exp `match` block in that guide turned out not to compile: it wrote
  arms as `Left(l)  handle-error(l)`, one line per arm, which sweet-exp reads as
  a single list. The four other guides with sweet `match` blocks already use the
  correct pattern-line-then-body-line shape; this guide's are now fixed to match,
  and its `#{NonExhaustive}` marker updated to the non-deprecated `#fx{...}`.
