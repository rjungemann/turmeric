# tur run silently ignores parameterized attributes and interpolated backticks

**Summary:** `tur run` skips any Justfile attribute carrying an argument list,
so `[confirm("...")]` executes its recipe with no prompt; separately,
backticks inside `{{ }}` expand to the empty string instead of being refused.

**Severity:** high for the `[confirm]` case (a destructive recipe the author
gated behind a prompt runs unprompted), medium for the rest (silent wrong
value where an error is expected).

Found 2026-08-30 while re-verifying
[docs/upcoming/tur-run-feature-audit-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/tur-run-feature-audit-plan.md).
Reproduced against `./build/tur` v0.41.0 on darwin.

## 1. Parameterized attributes are skipped, not refused

```just
[confirm("This will delete everything. Continue?")]
danger:
  echo "DESTRUCTIVE ACTION RAN WITHOUT PROMPTING"
```

```
$ tur run danger < /dev/null
echo "DESTRUCTIVE ACTION RAN WITHOUT PROMPTING"
DESTRUCTIVE ACTION RAN WITHOUT PROMPTING
```

Expected: the same "unsupported Justfile feature ... recipe attribute
[confirm]" refusal that bare `[confirm]` correctly produces.

Also affected, all silently dropped: `[group('build')]`, `[doc("...")]`,
`[extension('.sh')]`, and every other attribute with parentheses.

### Root cause

`src/compiler/justrun.c:331-358`.  The parser takes the text between `[` and
the first `]` and compares the **whole** span with `strcmp`:

```c
if (strcmp(attr, "unix") == 0 ||
    strcmp(attr, "windows") == 0 || strcmp(attr, "no-cd") == 0 ||
    strcmp(attr, "no-exit-message") == 0 ||
    strcmp(attr, "confirm") == 0 ||
    jr_starts_with(attr, "group:")) {
```

For `[confirm("msg")]`, `attr` is `confirm("msg")`, which matches nothing.
Control falls out of the `if (*p == '[')` block and the line is treated as an
ordinary non-recipe line, i.e. discarded.

The `jr_starts_with(attr, "group:")` arm is dead code: `just` has no `group:`
syntax, only `[group('name')]`, so it cannot fire on a real Justfile.

### Fix direction

Split the attribute name from its argument list (scan to `(` or `]`) and match
on the name.  Then make the unknown case **refuse** rather than fall through —
today any attribute not in the hardcoded list is a silent no-op, so every
attribute `just` adds upstream will land here as one too.

## 2. Backticks inside `{{ }}` expand to empty

```just
show:
  echo "inline {{ `echo xyz` }}"
```

```
$ tur run show
echo "inline "
inline
```

Expected: the "backtick command substitution" refusal that the assignment-RHS
path already emits.

```just
REV := `echo abc123`     # this one correctly errors
```

### Root cause

`src/compiler/justrun.c:370` checks for backticks only in an assignment RHS
(it looks for `:=` first).  The interpolation evaluator has no equivalent
check, and an unresolvable `{{ ... }}` body yields the empty string.

### Fix direction

Apply the same backtick detection to interpolation bodies.  Real backtick
evaluation is tracked as §2.3.4 of the audit plan, but the refusal should not
wait on it.

## 3. Why this survived the audit

None of `tools/just-vs-tur-run.sh`, `tests/run-tur-run-alias.sh`, or
`tests/run-tur-run-rhs-eval.sh` asserts that an unsupported construct is
*refused*.  The refusals are the load-bearing safety property of a partial
`just` implementation, and nothing tests them.  A table-driven "these must be
refused" case would have caught both bugs.
