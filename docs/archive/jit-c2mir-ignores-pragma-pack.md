# c2mir silently ignores `#pragma pack` and `__attribute__((packed))`

**`#pragma pack` RESOLVED 2026-07-31** in the `rjungemann/mir` fork, commit
`d7e19e8d` (`cmake/mir.cmake` repinned from `90633091`).
`__attribute__((packed))` is **still open** -- it goes through the attribute
path, not the pragma path -- but nothing in the corpus or the Darwin SDK
headers reaches it, so it is recorded here rather than left in
`docs/reported/`.

## Resolution

The packing is now tracked in the preprocessor -- the only stage that sees the
directive, since `check_pragma` consumes it and `pre()` runs to completion
before `parse()` starts. `out_token` stamps the packing in effect onto every
token it emits, which is the single choke point every parser-bound token passes
through and runs in preprocessing order, so the stamp stays correct when a
`pack(push)` spans an `#include` (how the Apple and Windows SDKs actually use
it). The parser lifts the stamp off the `struct`/`union` keyword onto the
`N_STRUCT`/`N_UNION` node, and `set_type_layout` / `aux_set_type_align` cap each
member's alignment at `min(its own alignment, pack)`.

Supported: `pack(N)`, `pack()`, `pack(push)`, `pack(push, N)`, `pack(pop)`,
`pack(pop, N)`, N a power of two in 1..16. An unrecognized form warns and is
ignored rather than silently changing layout.

Measured after the fix, on arm64 macOS:

| construct | `cc` | `tur jit` before | `tur jit` after |
|---|---|---|---|
| `#pragma pack(push, 4)` | 12 | 16 | **12** |
| `__attribute__((packed))` | 12 | 16 | 16 (still open) |
| `mach_msg_context_trailer_t` | 60 | failed the SDK assert | **60** |
| `mach_msg_mac_trailer_t` | 68 | failed the SDK assert | **68** |

(The two "before" Mach sizes are recorded as the assertion outcome rather than
a number: once `__arm64__` was restored the SDK's `_Static_assert` rejected the
layout before anything could print a `sizeof`, and the pre-fix values were
never measured directly.)

The Mach trailers now satisfy the SDK's own `_Static_assert`s, which is what
had been forcing the `image-*` fixtures to the cc fallback. Full JIT corpus:
**2414 passed, 0 failed**, with cc fallbacks down from 31 to 28 (the three
`image-*` fixtures now JIT).

---

## Original report follows

**Severity: medium-high (silent wrong layout, no diagnostic).** Unlike every
other c2mir gap on the JIT path, this one does not fail the compile and take
the cc fallback -- it *succeeds* and lays the struct out differently from the
host compiler. Any JIT'd code that reads or writes a packed struct sees wrong
offsets, with no warning beyond an `unknown pragma`.

Found 2026-07-30 on arm64 macOS while investigating
[jit-macos-apple-sdk-headers-force-cc-fallback.md](jit-macos-apple-sdk-headers-force-cc-fallback.md).

## Repro

```
(defn probe [] : int
  ```c
  #pragma pack(push, 4)
  typedef struct { unsigned int a; unsigned long long b; } p_pragma_t;
  #pragma pack(pop)
  typedef struct { unsigned int a; unsigned long long b; } __attribute__((packed)) p_attr_t;
  typedef struct { unsigned int a; unsigned long long b; } p_nat_t;
  printf("pragma_pack4=%d attr_packed=%d natural=%d\n",
         (int)sizeof(p_pragma_t), (int)sizeof(p_attr_t), (int)sizeof(p_nat_t));
  return 0;
  ```)
(defn main [] : int (probe))
```

| construct | `cc` | `tur --enable=jit jit` |
|---|---|---|
| `#pragma pack(push, 4)` | 12 | **16** |
| `__attribute__((packed))` | 12 | **16** |
| natural alignment | 16 | 16 |

The JIT run takes **no** fallback (no TUR-W0070), so those are c2mir's own
numbers, not cc's. c2mir emits only `warning -- unknown pragma`, which is lost
in the noise of the dozens it already emits per compile.

## Why this matters more than the parse-error classes

The rest of the macOS JIT gaps are *loud*: c2mir refuses the input and the
step-6 cc fallback produces a correct binary. Correctness is preserved and the
only cost is JIT coverage. This class is the opposite -- it compiles, runs, and
is wrong.

The parent report recorded this as finding 4 and filed it as dormant: "Verified
2026-07-30 that Turmeric's emitter produces neither construct, so nothing
exercises it today ... a constraint to respect, not an active defect." That
conclusion holds for the *emitter* but not for the JIT path as a whole, because
two other sources reach c2mir:

1. **User inline C.** The repro above is ordinary inline C in a `.tur` file --
   a thing the language invites people to write. Nothing rejects it.
2. **The system headers themselves.** `mach/message.h:291` opens
   `#pragma pack(push, 4)` and closes it at `:625`; every Mach message trailer
   struct is declared inside that span. c2mir lays them out at natural
   alignment, so `mach_msg_context_trailer_t` comes out 64 rather than the 60
   the SDK asserts.

## Interaction with the `__arm64__` fix (important)

Until 2026-07-30 this was invisible on Darwin for an accidental reason. MIR's
Apple prelude spelled `#define __arm64__` with no replacement list
(`c2mir/aarch64/mirc_aarch64_linux.h:135`), so the SDK's `#if __arm64__` at
`mach/port.h:100` was an "empty preprocessor expression" error and the block it
guards -- the definition of `xnu_static_assert_struct_size` -- was never taken.
**The SDK's own struct-size assertions were disabled**, so c2mir's wrong layout
passed unremarked.

`src/jit_engine.c`'s `JIT_PRELUDE` now restores `__arm64__` to `1`, which
re-arms those assertions. `mach/message.h:543` and `:569` now fail with
`static assertion failed: "struct changed size unexpectedly"` -- correctly --
and the affected programs take the cc fallback instead of running on a bad
layout. That is a strict safety improvement even though it does not reduce the
fallback count.

## Fix directions

1. **Implement `#pragma pack` / `packed` in c2mir.** The real fix. It is a fork
   change (we already carry `rjungemann/mir` for three other patches, see
   `cmake/mir.cmake`), and it is not small: the layout code has to thread a
   current-packing value through struct/union member offset assignment and
   honour push/pop. This is the only option that lets JIT'd code touch packed
   structs correctly.
2. **Refuse rather than miscompile.** Much cheaper and strictly better than
   today: make c2mir (or a pre-scan in `jit_engine.c`) treat `#pragma pack`
   with a non-default argument and `__attribute__((packed))` as unsupported,
   and fail the compile so the program takes the cc fallback. Converts a silent
   wrong answer into a correct-but-slower one. Worth doing even as a stopgap
   ahead of option 1.
3. **Do not rely on the SDK assertions.** They caught the Mach case only
   because Apple happens to assert those sizes. Nothing catches a user's packed
   struct in inline C.

## Note for the emitter

The parent report's guidance still stands and is now load-bearing rather than
advisory: **do not start emitting packed structs from codegen** without
resolving this first. Today the emitter produces neither construct, which is
the only reason the blast radius is limited to hand-written inline C and the
system headers.

## Verification

The repro above should print identical numbers for `cc` and `jit`. Today it
does not.
