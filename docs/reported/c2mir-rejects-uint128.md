# c2mir cannot parse `__uint128_t`, so inline C using it never reaches the JIT

**Severity: medium.** `tur jit` declines any program whose inline C writes
`__uint128_t`: the engine's parse fails, `TUR-W0070` fires and the cc path takes
over, so the answer is still right -- just compiled the slow way, with a warning,
and with the engine silently unused for that program. On aarch64 the same gap
also makes a *system* header unparseable, which is the mechanism behind
[jit-xopen-source-guard-inert-on-glibc](jit-xopen-source-guard-inert-on-glibc.md)
and what stranded the v0.60.0 release.

Found 2026-10-03 while diagnosing that release failure.

## Repro

On any host with the JIT engine (measured on `ubuntu-24.04-arm`, glibc 2.39,
gcc 13):

```turmeric
(defn widen [x : int] : int
  ```c
  __uint128_t v = (__uint128_t)x;
  v = v * 3u;
  return (int64_t)v;
  ```)

(defn main [] : int
  (println (widen 14))
  0)
```

```sh
./build/tur jit u128.tur
# <tur-jit>:2539:21: syntax error on identifier (expected ';'):
# <tur-jit>:2539:21: syntax error on identifier (expected '<statement>'):
# <tur-jit>:2545:1:  syntax error on int (expected '<statement>'):
# <tur-jit>:2538:33: unfinished compound statement
# tur: warning: TUR-W0070: jit engine could not compile this program; falling back to the cc path
# 42
```

The answer (42) is correct -- that is the cc path. `TUR_JIT_DUMP_C=<path>` writes
the exact text handed to c2mir, and a `<tur-jit>:LINE` maps into that file
(`src/main.c:4962`).

**Read the evidence differentially.** On aarch64 every `tur jit` run also prints
a `sys/user.h:30` error, including a plain hello-world, so that line is baseline
noise here and says nothing about this program. The four `<tur-jit>:2539`-area
errors above are what no other program produces, and they sit exactly at the
inline-C body. On a platform without the header problem the baseline is quiet and
these are the only errors.

## Root cause

Not pinpointed inside c2mir. What is established is the behaviour: the token is
rejected at parse time, in a position where a type name is expected, and the
generated unit itself never writes `__uint128_t` anywhere -- so nothing else in
the JIT path depends on support existing today.

Prior art says support may be partial rather than absent:
[`docs/archive/history/jit-arm64-uint128-align-struct-layout-skew.md`](../archive/history/jit-arm64-uint128-align-struct-layout-skew.md)
records a fix in the fork for `__uint128_t` *alignment and struct layout* on
arm64, which implies the type is modelled somewhere downstream of the parser.
Worth reading before assuming the frontend has to learn it from scratch.

## Fix directions

- The change belongs in the vendored fork under `external/mir/`, not in this
  repo's sources. `external/mir/VENDORED.md` says how to change MIR and
  `tools/update-mir.sh` re-syncs the copy; never edit `external/mir/` by hand.
- Start from the prior-art report above -- if layout and alignment are already
  handled, the gap may be confined to the c2mir parser accepting the type
  specifier.
- A fixture belongs with the fix: the program above under `tests/fixtures/`,
  exercised on the JIT path, so the gap cannot silently reopen. Note that a
  fixture asserting only *output* would pass today via the cc fallback -- it has
  to assert the absence of `TUR-W0070`, the way `release.yml`'s archive JIT check
  does.
- Until it lands, the aarch64 header consequence can be sidestepped on its own;
  see fix direction 1 of the sibling report.
