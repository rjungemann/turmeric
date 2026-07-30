# Missing `tur_hamt_hash_xxh64` prototype corrupts the call under `tur jit`

**Severity: high** (silent memory corruption -> SIGSEGV/SIGBUS; wrong-code class,
not merely a diagnostic gap). Affects the JIT engine only; the `cc` path is
correct by suppression, not by construction.

## One-line summary

The emitted C calls `tur_hamt_hash_xxh64` with no prototype in scope. `cc` is
told to tolerate that (`-Wno-error=implicit-function-declaration`), and the
implicit declaration happens to lower to the right ABI. `c2mir`/MIR lowers the
unprototyped call differently: the arguments arrive **shifted by one register**,
so `xxh64` dereferences a code address using a stack address as its length and
runs off the end of the mapped region.

## Repro

arm64 macOS (Apple M2, Darwin 27.0.0), `-DTUR_JIT=ON` build:

```sh
./build-turjit/tur --enable=jit jit tests/fixtures/map-multiword-struct-key/input.tur   # SIGBUS/SIGSEGV
./build-turjit/tur run              tests/fixtures/map-multiword-struct-key/input.tur   # correct
```

Two fixtures fail this way, and they are exactly the two whose `Hash`/`MapKey`
instance bodies call `tur_hamt_hash_xxh64` from inline C:

- `tests/fixtures/map-multiword-struct-key`
- `tests/fixtures/set-multiword-struct-element`

`map-multiword-struct-value`, `vec-multiword-struct-element`,
`vec-multiword-struct-mutate` and `vec-multiword-struct-eq` all pass -- they
never reach the hash entry point. The multiword-struct framing is a red
herring; the discriminator is the undeclared call.

## Root cause

`tur emit-c` on the fixture produces:

```c
extern void * tur_hamt_box_key(void *, int64_t);        /* ~line 3896 */
extern bool   tur_hamt_box_key_eq(int64_t, int64_t);    /* ~line 3897 */
...
static int64_t __inst_Hash_hash_Point(tur_adt_Point p) {
        return (int64_t)tur_hamt_hash_xxh64((const void*)&p, sizeof(p));   /* ~line 5647 */
}
```

`tur_hamt_box_key` and `tur_hamt_box_key_eq` are declared. Its sibling
`tur_hamt_hash_xxh64` -- declared in `src/runtime/hamt.h:337` as
`uint64_t tur_hamt_hash_xxh64(const void *data, size_t len)` -- is simply
omitted from the emitted declaration set.

lldb at the fault:

```
thread #2, EXC_BAD_ACCESS (code=1, address=0x101968000)
frame #0: tur`tur_hamt_hash_xxh64 + 104
->  ldp    x17, x2, [x0]
```

and breaking at function entry shows the shift directly:

```
x0 = 0x000000010015d6f8  tur`tur_hamt_hash_xxh64     <- the callee's OWN address, passed as `data`
x1 = 0x0000000173e06e90                              <- the real `&p`, passed as `len`
```

So `data` is a code address and `len` is a stack address (~6.2 GB), and the
xxh64 stripe loop walks straight off the end of the page. The faulting address
`0x101968000` is page-aligned -- the first byte past a mapped region.

The precise mechanism is Apple's arm64 ABI rule that **anonymous variadic
arguments are passed on the stack**, not in registers. c2mir treats a call with
no prototype in scope as all-anonymous-variadic, so it places `&p` and `16` on
the stack and `xxh64` reads whatever junk is in `x0`/`x1`. x86-64 SysV passes an
unprototyped call's arguments in the same registers as a prototyped one, which
is exactly why the Linux baseline never saw this.

Isolating the declaration form confirms it -- same body, three spellings:

| declaration in scope | JIT | cc |
|---|---|---|
| `extern int64_t f(const void*, int64_t);` | -4696535041872477288 | same |
| `extern int64_t f();` (no prototype) | **3702258817787715709** | -4696535041872477288 |
| `extern int64_t f(const void*, int64_t, ...);` | -4696535041872477288 | same |

It is specifically the *fully unprototyped* form; an explicit variadic with two
named parameters is fine.

**The struct is incidental, and this is a wrong-answer bug as well as a crash.**
Without a struct argument there is no fault, but the JIT still silently computes
a different hash (`h=-1429151107` vs `1107575704`). And the implicit declaration
returns `int`, so the 64-bit hash is **already truncated to 32 bits on the `cc`
path, on every host** -- that part is a live, host-independent defect today,
independent of the JIT.

This is already a known-and-suppressed defect on the `cc` path.
`src/main.c:4913-4917` says so in as many words, naming this exact symbol:

> `-Wno-error=implicit-function-declaration` is a SEPARATE, still-open concern
> (e.g. an emitted inline-C call to `tur_hamt_hash_xxh64` with no in-scope
> prototype) and stays.

The JIT has no equivalent escape hatch, and c2mir's lowering of an unprototyped
call does not coincide with the real ABI the way clang's does. A knowingly
tolerated warning on one backend is memory corruption on the other.

## Proof, both directions

Minimal probes through the spike harness
(`./build-jit-spike/tools/jit-spike/tur-jit-spike --eager --quiet`):

```c
/* WITH the prototype -> correct result under both cc and MIR */
extern uint64_t tur_hamt_hash_xxh64(const void *p, size_t n);
typedef struct { int64_t x; int64_t y; } Point;
static int64_t hashit(Point p) { return (int64_t)tur_hamt_hash_xxh64((const void*)&p, sizeof(p)); }
```

Deleting only the `extern` line makes the same file fail. And splicing one
declaration into the fixture's own emitted TU fixes it end to end:

```sh
./build-turjit/tur emit-c tests/fixtures/map-multiword-struct-key/input.tur > mm.c
sed 's|^extern void \* tur_hamt_box_key(void \*, int64_t);|extern uint64_t tur_hamt_hash_xxh64(const void *, size_t);\n&|' mm.c > mm-fixed.c
./build-jit-spike/tools/jit-spike/tur-jit-spike --eager --quiet mm-fixed.c
# 99 / 77 / 0 / eq / ne  -- byte-for-byte the expected.stdout
```

Note the earlier hypothesis this displaces: it is **not** MIR mishandling
`&param` for a register-passed struct on arm64. That construct round-trips
correctly under MIR when the callee is prototyped.

## Fix directions

1. **Direct fix:** emit `extern uint64_t tur_hamt_hash_xxh64(const void *, size_t);`
   into the preamble alongside the existing `tur_hamt_box_key` /
   `tur_hamt_box_key_eq` declarations. One line; verified above to fix both
   fixtures. Note this changes the preamble, so it regenerates every
   `tests/fixtures/*/expected.c` snapshot -- per CLAUDE.md that regen belongs in
   the same commit.
2. **Systemic fix (preferred, larger):** the reason this was reachable at all is
   that the inline-C-facing runtime surface has no single declared boundary --
   which is exactly what plan item **S2** ("runtime-as-library boundary: one
   header listing every runtime symbol the generated C may reference") is for.
   Landing S2 makes this class unrepresentable rather than fixing one instance.
   Until then, dropping `-Wno-error=implicit-function-declaration` and fixing
   the fallout would surface any remaining siblings at cc-compile time instead
   of at JIT runtime.

Whichever is taken, the `-Wno-error=implicit-function-declaration` suppression
in `src/main.c:4917` should be revisited: it is currently hiding a wrong-code
bug on a second backend, not just a style warning.

## Platform note

The *crash* is arm64-macOS-specific, because it depends on Apple's
stack-passing rule for anonymous variadic arguments. The *missing declaration*
is host-independent and is already truncating this hash's return value to 32
bits on the `cc` path everywhere. Anyone enabling the JIT on x86-64 with a
different argument shape can hit the register-mismatch class too.

## Provenance

Found during the arm64 macOS re-validation of the JIT engine (Apple M2, Darwin
27.0.0, Apple clang 21.0.0), `tur` v0.32.2, MIR pin `41ff4d94`. See section 20
of [../upcoming/jit-engine-j0-findings.md](../upcoming/jit-engine-j0-findings.md).
