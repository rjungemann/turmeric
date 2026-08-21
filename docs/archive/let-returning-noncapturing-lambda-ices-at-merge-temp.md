# ICE: a `let` whose result is a NON-capturing lambda trips the repr-shadow guard

**Severity: medium.** Split out of
`docs/archive/httpd-mw-recover-unblocked-but-unwritten.md` (repro A) on
2026-08-21, when the other three blockers in that report were fixed and
`mw-recover` shipped. This one survives on its own.

## Repro

```turmeric
(defn wrap [] : int
  (let [_x 1]
    (fn [c : int] : int c)))
(defn main [] : int 0)
```

```
tur: internal error (ICE): a representation decision disagrees with repr_of at merge-temp.
  repr-shadow merge-temp result type=(fn [int] : int) want=fat-handle got=carrier-i64 cty=int64_t own=int64_t
```

## What narrows it

Three conditions, all necessary:

| Variant | Result |
| --- | --- |
| the repro above | **ICE** |
| lambda body uses `_x` (so it captures) | compiles |
| `(do ...)` instead of `(let ...)` | compiles |
| lambda returned directly, no wrapping form | compiles |

The return type of the enclosing `defn` is *not* a factor -- `: int` and
`: ptr<void>` both ICE. So the trigger is specifically **a `let` merge
temp whose value is a captureless lambda**: the lambda lowers to a bare
function pointer (`carrier-i64`), the merge temp was already decided as
`fat-handle`, and the shadow guard catches the disagreement.

## Why it is not just a guard question

Under `TUR_REPR_NO_SHADOW_ICE=1` the repro compiles, runs, and exits 0,
so for this shape the disagreement is benign. That is not a licence to
relax the guard. `docs/archive/let-bound-noncapturing-lambda-segfaults-as-fn-arg.md`
is the same family -- a bare pointer reaching a consumer that wanted a
fat handle -- and there it was **silent** and segfaulted. The guard is
reporting a real inconsistency.

## Fix direction

Make the value actually be a fat handle at the merge temp, rather than
teaching the guard to look away. The archived fix at the *argument*
boundary (`ensure_bare_fnptr_poly_shim`, a signature-keyed adapter that
wraps a bare function pointer into a `{shim, NULL}` fat pair) is the
shape to reuse: apply it when a `let`/merge temp's declared repr is
`fat-handle` and the incoming value is a captureless closure. The
alternative -- make `repr_of` agree that a captureless closure is a bare
pointer -- means auditing *every* consumer to handle one, which is the
larger change and the one that produced the archived segfault.

See `docs/archive/repr-decision-function-plan.md` for the defect family.

## Guides to update when fixed

None -- no documented surface promises this shape today.

---

## Resolution (2026-08-21)

Fixed at the merge temp, as the report directed -- but the direction of the
disagreement was the opposite of what the filing assumed, and that changes the
fix.

### The value was already fat; the TEMP was thin

The report read `want=fat-handle got=carrier-i64` as "the lambda lowers to a
bare function pointer where a fat handle was wanted", and proposed wrapping the
bare pointer with `ensure_bare_fnptr_poly_shim`. Emitting the repro with the
guard downgraded shows the reverse -- the tail builds a perfectly good fat box:

```c
static void * wrap() {
        int64_t (*__t160)(int64_t);           /* <- the temp: THIN */
        {
            void *__t162 = malloc(sizeof(void *) + 2 * sizeof(int64_t));
            int64_t *__t161 = (int64_t *)((char *)__t162 + sizeof(void *));
            __t161[0] = (int64_t)(intptr_t)__tur_fatshim1;   /* <- the value: */
            __t161[1] = (int64_t)(intptr_t)__fn_1333;        /*    FAT        */
            void *__t163 = __t161;
            __t160 = (int64_t)(intptr_t)(__t163);
        }
        return __t160;
}
```

So there was no bare pointer to wrap. `repr_of` was right and the declaration
was wrong.

### It was not only a shadow complaint

`TUR_REPR_NO_SHADOW_ICE=1` makes the repro "compile, run, and exit 0", which is
what led the report to call the disagreement benign for this shape. It is not.
Give the lambda a body and actually call the result and the same program emits:

```
warning: assignment to 'int64_t (*)(int64_t)' from 'long int'
         makes pointer from integer without a cast [-Wint-conversion]
```

which is a **hard error under GCC >= 14** -- the same
`-Wint-conversion`-away-from-broken family as the other carrier-bridge seams.
The ICE was reporting a real defect that the exit-0 repro happened not to show.

### Root cause: two sites, two different questions

- `emit_temp_decl` picks thin-vs-fat off `type.as.fn.boxed` -- a fact about the
  **type**.
- Stage-2 tail normalization picks it off `fn_result_type_is_fat_normalized()`
  -- a fact about the **position**: a fn value reaching a result slot is boxed
  into a fat pair whether or not its type carries the flag.

For a captureless lambda in a `let` tail those disagree. `do` and the direct
return escaped because only the `let` path calls
`bridge_control_result_int_ptr` -> `control_result_temp_ctype`, which is where
the shadow check lives; `do` declared its temp thin too, it just was not
checked.

### Fix

`merge_temp_fn_is_fat()` asks `repr_of(&resolved, REPR_POS_RESULT)` -- the
arbiter `repr-decision-function-plan` exists to make authoritative -- instead of
re-deriving a second answer. `emit_control_result_temp_decl` declares the temp
`void *` when it says fat, and `control_result_temp_ctype` mirrors that branch
so the two stay byte-identical. Result:

```c
void * __t161;
...
__t161 = __t164;        /* plain pointer assignment; no cast, no warning */
```

The spelling is `void * name` rather than `void *name` deliberately: the
generic `emit_type_c_name` path already reaches this declaration for a
type-level boxed fn, and a different spacing forked four snapshots
(`arrow-compose-float`, `fat-shim-void-ptr-arrow-compose`,
`load-inside-defmodule-injects-names`, `sf-compose-typed`) on whitespace alone.
Matching it means **zero** fixtures regenerated.

### Tests

`tests/fixtures/let-tail-noncapturing-lambda-fat-temp/` carries the original
`: int` repro (defined, never called -- the ICE fired while emitting the body,
and its value is a heap pointer, so printing it would make the fixture
machine-dependent) plus the load-bearing variant that returns the closure and
calls it, where a mistyped temp is a wrong answer rather than a diagnostic. The
three contrast arms from the report's narrowing table (capturing, `do`, direct)
are there so the fix cannot silently reroute them. `expected.c` pins the
`void *` temp; the emitted TU has zero `-Wint-conversion` warnings.

Suite: 2689 passed, 0 failed. `check-repr-decision-ratchet.sh` still passes at
its 25 pinned rows.

### Note on the archived sibling

`let-bound-noncapturing-lambda-segfaults-as-fn-arg` (the argument boundary)
really was a bare pointer reaching a fat consumer, and its
`ensure_bare_fnptr_poly_shim` fix stands. The two are the same *family* -- a
representation decided twice -- but not the same direction, which is why
reusing that shim here would have boxed an already-boxed value.
