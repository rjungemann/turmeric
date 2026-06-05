# `^fat` parameters are emitted as `void *`, warning `-Wint-conversion` in inline-C bodies

**One-line summary:** A `defn` parameter declared `^fat` is emitted into the C
function signature as `void *`, but every other Turmeric value crosses the C
boundary as `int64_t` -- so an inline-C body that handles the `^fat` parameter
with the idiomatic `int64_t` handle pattern (`p->slot = f;`, `int64_t h = f;`)
triggers `-Wint-conversion` ("makes integer from pointer without a cast").

**Severity:** Ergonomics gap / latent build break. Not a miscompile -- the
implicit `void *`->`int64_t` conversion is value-preserving on LP64, and the
caller already round-trips the same bits through `(void *)(intptr_t)...` -- but
(1) it would become a hard error under `-Werror`, (2) it is an ergonomic trap:
the *natural* inline-C handle idiom warns, forcing every inline-C `^fat`
consumer to hand-write `(int64_t)(intptr_t)f`, and (3) it is inconsistent with
the rest of the value ABI, where handles (including `:ptr<void>`-carried fat
closures everywhere else) are `int64_t`.

## Minimal repro

```turmeric
(defn store [^fat f] : int
  ```c int64_t h = f; return (int)h; ```)

(defn id [x : int] : int x)

(defn main [] : int
  (println (store id))
  0)
```

Build/run (`tur run /tmp/fatc.tur`):

```
.../_tmp_fatc_tur.c:4457:21: warning: initialization of 'int64_t' {aka 'long int'}
  from 'void *' makes integer from pointer without a cast [-Wint-conversion]
```

The emitted C:

```c
static int64_t store(void * f) {          /* <- ^fat param typed void * */
    int64_t h = f; return (int)h;         /* <- warns: void * -> int64_t */
}
...
printf("%lld\n", (long long)(store((void *)(intptr_t)(__t26))));
                                  /* ^ caller pre-casts an int64_t handle back to void * */
```

**Observed:** the `^fat` param is `void *`; assigning it to an `int64_t`
(the idiomatic handle representation, and what the caller started from) warns.
**Expected:** an inline-C body can treat a `^fat` handle as `int64_t` -- the
same as every other Turmeric value -- without a manual cast and without a
warning.

This is also the (benign) warning observed while validating the resolved
`arrow-instance-apply` fixture, whose `pair-fn-arg` helper does
`p->e1 = f;` with a `^fat` `f` and an `int64_t` slot:

```
.../tests_fixtures_arrow-instance-apply_input_tur.c:5207:9: warning:
  assignment to 'int64_t' from 'void *' makes integer from pointer without a cast
   5207 |   p->e1 = f; p->e2 = arg; return (int64_t)(intptr_t)p;
```

## Root cause

`src/compiler/emit_fns.c`, the parameter-signature loop (`emit_fns.c:436-481`).
A `^fat` parameter carries `param_types[i] == :ptr<void>`, and the `else`
branch emits its C type via `emit_type_c_name(ctx, param_ty)`
(`emit_fns.c:468`), which renders `:ptr<void>` as `void *`. The same loop emits
the CPS wrapper signature (`emit_fns.c:678-682`), so the wrapper path has the
identical shape.

Nothing downstream needs the `void *` spelling: the caller already emits the
argument as `(void *)(intptr_t)<int64_t-handle>` (i.e. it *starts* from an
`int64_t` handle and casts up to `void *` only to match this signature), so the
`void *` choice is pure friction at the inline-C boundary -- the one place a
human writes the body and naturally reaches for `int64_t`.

The existing comment at `emit_fns.c:454-460`
(referencing `bare-fat-sink-poly-box-slot0-int64-mismatch.md`) already
establishes that a `^fat` param is "a fat-closure *carrier* handle, never a
by-value fn"; a carrier handle's canonical C type is `int64_t`, not `void *`.

## Proposed fix directions

1. **Emit `^fat` params as `int64_t`.** In the `emit_fns.c` param loop, special-
   case `fd->params[i]->is_fat` to emit `int64_t` (mirroring the `TY_FN`
   branch at `emit_fns.c:441-444`), in both the direct signature and the
   `__cps` wrapper signature. Then drop the now-redundant `(void *)(intptr_t)`
   up-cast at the call site (it already lowers from an `int64_t` source), or
   leave it harmless as `(int64_t)(intptr_t)`. This makes the idiomatic
   inline-C body (`p->e1 = f;`, `int64_t h = f;`) warning-clean with no caller
   change required if the cast is simply normalized to `int64_t`.

2. **If `void *` must stay** (e.g. some path relies on pointer typing): cast at
   the *use* inside generated inline-C splicing is not possible (the body is
   user-authored), so the only alternative is to document the requirement and
   have inline-C authors write `(int64_t)(intptr_t)f`. This is strictly worse
   than #1 and is recommended only if #1 surfaces a real dependency on the
   pointer type.

## Validation of a fix

- The minimal repro above compiles with **no** `-Wint-conversion` warning and
  prints the handle round-tripped through inline-C.
- `tests/fixtures/arrow-instance-apply` still prints `42 / 42 / 1007` and its
  generated C no longer warns on the `pair-fn-arg` slot assignment.
- The full suite stays green (`bash tests/run.sh`, zero `FAIL`), and the
  bare-fat fixtures referenced by `emit_fns.c:454-460`
  (`bare-fat-sink-poly-box-slot0-int64-mismatch`) are unchanged.
- Worth adding a tiny regression fixture: a `^fat`-param `defn` with an
  inline-C body that stores `f` into an `int64_t` slot, snapshotting the
  warning-free `expected.c`.
