# A generator's thunk call site returns `void *` where the closure returns `int64_t`

**Severity: low-medium.** Undefined behavior, not a wrong answer. The mismatch
is ABI-benign on LP64 -- both types are 8 bytes returned in the same register
-- so the program prints the correct result and only
`-fsanitize=function` sees it. It is a latent hazard: nothing guarantees a
future target, or a sufficiently aggressive optimizer, keeps treating the two
as interchangeable.

Found by the nightly `Fuzz` workflow, `type-fuzz-src` seed **20261003**,
case 376, as `BUG_fnptr_trap`
(tags `closure_ret,gbody,gid,scalar_bool,thin_hof,thunk`).

## Minimal repro

Four forms, reduced from the generator's 11:

```turmeric
(defn mk [x : bool] : (fn [] bool) (fn [] x))
(defn thunk [B] [v : B] : (fn [] B) (fn [] v))
(defn gbody [A] [x : A] : A
  ((thunk (gen-unwrap (gen-next (gen [] (yield ((fn [] x)))))))))
(defn main [] : int
  (println ((gbody (mk true))))
  0)
```

```sh
# correct answer, no diagnostic
tur build repro.tur -o repro && ./repro            # prints: true

# the same program under the function-pointer check
CC=<a clang with -fsanitize=function> \
TUR_CC_FLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -fsanitize=function -fsanitize-trap=function" \
  tur build repro.tur -o repro && ./repro          # dies: SIGTRAP, no output
```

## Root cause

With `-fsanitize=function` and no `-fsanitize-trap`, clang names it exactly:

```
runtime error: call to function __fn_19 through pointer to incorrect
               function type 'void *(*)(void *)'
note: __fn_19 defined here
```

In the emitted C the closure is defined returning the carrier:

```c
static int64_t __fn_19(void * __env_p_22) {
        struct __env_21 *__env___env_21 = (struct __env_21 *)__env_p_22;
        return __env___env_21->x;
}
```

and the generator's resume body calls it through a `void *`-returning thunk:

```c
typedef void * (*tur_thunk_void___t)(void *);
...
int64_t __ps_200 = ((int64_t)(intptr_t)(
    (*( tur_thunk_void___t *)((void *)(intptr_t)(__g->__call_head_24)))
        ((void *)(intptr_t)(__g->__call_head_24))));
```

So it is purely a **return-type** disagreement -- `void *` at the call site
versus `int64_t` at the definition. The parameter lists match, and the result
is immediately cast back through `(int64_t)(intptr_t)`, which is why the value
survives. The call site picked the generic `void *` thunk shape while the
closure body was emitted against the type variable's `int64_t` carrier;
whichever of the two is authoritative, they are chosen independently here.

## Which ingredients are load-bearing

Measured by reduction; `plain` is the default build, `fnsan` adds the check.
Every row prints `true` under `plain`.

| variant | fnsan |
| --- | --- |
| the four forms above | **TRAP** |
| ... with the generator removed (`(thunk x)`) | no trap |
| ... yielding `x` instead of `((fn [] x))` | no trap |
| ... yielding `(let [l x] l)` | no trap |
| ... with `thunk` removed (yield's value returned directly) | no trap |
| ... with a thin HOF added back | TRAP (as found) |
| ... with a generic identity pass-through added back | TRAP (as found) |

So three things must coincide: a **generator yield**, an **immediately-applied
closure** as the yielded expression, and a **generic thunk** wrapping the
unwrapped result. The `gid` and `thin_hof` crossings in the original tags are
incidental -- the finding reproduces without them.

## Fix directions

1. **Emit the thunk call site at the closure's own return type.** The call
   already casts the result back through `(int64_t)(intptr_t)`, so the narrow
   change is to select the `int64_t`-returning thunk typedef here rather than
   `tur_thunk_void___t`. Cheapest if the generator lowering has the callee's
   emitted return type to hand at that point.
2. **Emit the closure returning `void *`** so it matches the generic thunk.
   Consistent with the call site, but it moves the cast rather than removing
   it, and the closure's return type is the tyvar's carrier everywhere else.
3. **Make the thunk typedef family carrier-parameterized** so both sides
   derive the same type from one place, which is what would stop the next
   instance of this rather than this one.

## A caveat about the pinned probe

This defect is invisible without `-fsanitize=function`: the program builds,
runs, and prints the right answer. Its `--known-probes` row therefore reports
`FIXED` on any box where fnsan is unavailable -- notably stock macOS, where
Apple clang does not provide it and the harness prints
`fnsan: UNAVAILABLE`. **Do not retire this row on a `FIXED` from a run whose
banner does not say `fnsan: ARMED`.** Reproduce with a clang that has the
check (Homebrew LLVM works: `CC=$(brew --prefix llvm)/bin/clang`).

Related: on arm64 the trap arrives as SIGTRAP rather than SIGILL, which the
harnesses misclassified as `BUG_toolchain_other` until
`tests/fuzz_arm.py` learned both statuses.
