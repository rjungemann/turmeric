# `spices/secret`: linear key material + real crypto hygiene

> **Status:** proposed (2026-08-17). **Track:** post-v1 -- nothing on the v1
> line depends on this; it is written down so the design survives.
> **Type:** spice (in `../turmeric-spices/`), plus one optional stdlib fix
> (`random.tur`).

## 0. Summary

Turmeric currently has **zero** security-grade memory or randomness
primitives: `stdlib/random.tur` is libc `rand()` seeded with `time(NULL)`,
`stdlib/digest.tur` has SHA-256/MD5 but no HMAC and no constant-time
compare, `raw-memset` lowers to plain `memset` (which the C compiler may
elide), and greps for `explicit_bzero` / `mlock` / `getrandom` /
`memset_s` over `src/` and `stdlib/` return nothing.

This plan proposes a `spices/secret` spice inspired by Go's
`runtime/secret` (Go 1.26, `GOEXPERIMENT=runtimesecret`) but designed
around what Turmeric can actually guarantee. The headline idea: Go
guarantees erasure at the *runtime* level (registers + stack scrubbed by
the scheduler); Turmeric cannot make that promise (the downstream C
compiler owns register allocation), but Turmeric has something Go does
not -- **substructural types**. A `:linear` opaque `Secret` buffer turns
"you must wipe your key material" from a code-review item into a
**compile-time error**. Combined with best-effort runtime hygiene
(`explicit_bzero`-class wipes, `mlock`, `MADV_DONTDUMP`, constant-time
compare, a real CSPRNG) this is a credible, honest offering at the same
guarantee tier as libsodium -- with a type-system upgrade libsodium
cannot express.

## 1. What Go `runtime/secret` is, and why we do not clone it

Reference points (verified 2026-08-17):

- Shipped in **Go 1.26** behind `GOEXPERIMENT=runtimesecret`;
  linux/amd64 and linux/arm64 only; not covered by the Go 1
  compatibility promise. Proposal: golang/go#21865 (zx2c4/WireGuard).
- API is two functions: `secret.Do(f func())` and `secret.Enabled()`.
- `Do` guarantees: registers used by `f` erased before `Do` returns;
  the goroutine stack used by `f` erased before `Do` returns (runtime
  switches to the system stack and `memclrNoHeapPointers`s the goroutine
  stack); heap allocations made inside `f` zeroed by the GC once
  unreachable (timing at the GC's discretion). Erasure happens even on
  panic; spawning a goroutine inside `f` panics.
- Explicitly NOT guaranteed: globals, swap, core dumps, copies made
  outside the `Do` call. On unsupported platforms `Do` silently
  degrades to a bare call.

Why Turmeric cannot copy the mechanism: we emit C and hand register
allocation and stack layout to `cc`. A post-call stack scrub (call a
`noinline` function that allocates a large frame and zeroes it, plus
inline-asm register clobbers) is achievable but is a **heuristic**, not
a guarantee -- exactly the tier libsodium, Rust `zeroize`, and Zig
`std.crypto.secureZero` occupy, and all three document the same
limitation. We should ship that tier honestly rather than claim Go-level
semantics we cannot deliver.

What we have that Go does not: `-Xsubstructural` linear types, already
proven in production shape by the sqlite spice
(`spices/sqlite/src/sqlite/db.tur`, `(defopaque Db :int :linear)` to
enforce open/close pairing). Linearity gives a *stronger* static story
than Go's dynamic one: the compiler rejects any path that drops a
secret without explicitly consuming (wiping) it.

## 2. Design

### 2.1 Core type

```turmeric
;; Owned, locked secret buffer. :linear => every control path must
;; consume it with exactly one of the consuming operations below.
(defopaque Secret :int :linear)
```

Allocation locks and marks the backing pages:

- `mlock(2)` -- keep it out of swap (fail-soft: record, do not abort,
  when `RLIMIT_MEMLOCK` denies it; expose `secret-locked?`).
- `madvise(MADV_DONTDUMP)` on Linux -- keep it out of core dumps.
- Allocation is a single malloc'd `{ int64_t len; unsigned char data[]; }`
  in the zlib-spice buffer style (`spices/zlib/src/tur/zlib.tur`,
  `__gzbuf`).

### 2.2 API sketch

```turmeric
;; -- construction ---------------------------------------------------
(defn secret-random!  [n : int]                    : result<Secret, cstr>)
(defn secret-of-bytes [p : ptr<void> n : int]      : result<Secret, cstr>)
  ;; copies then wipes the caller's buffer via secure-wipe-ptr!

;; -- consumption (linear: exactly one of these ends a Secret) -------
(defn secret-wipe!    [s : Secret]                 : void)
  ;; explicit_bzero-class wipe, munlock, free. THE destructor.

;; -- borrowing access (non-consuming) -------------------------------
(defn with-secret     [^borrow s : Secret
                       f : (fn [ptr<void> int] int)] : int)
  ;; scoped access; the pointer must not escape f (documented, and the
  ;; borrow checker already prevents the Secret itself escaping).

;; -- operations that never branch on secret data --------------------
(defn secret-eq?      [^borrow a : Secret ^borrow b : Secret] : bool)
  ;; constant-time compare (mbedtls_ct_memcmp or hand-rolled volatile
  ;; accumulate-xor loop)
(defn secret->hex     [^borrow s : Secret] : result<Secret, cstr>)
(defn secret-of-hex   [c : cstr]           : result<Secret, cstr>)
  ;; constant-time codecs; note the decode *output* is itself a Secret

;; -- interop --------------------------------------------------------
(defn secret-hmac-sha256 [^borrow key : Secret p : ptr<void> n : int]
  : result<Secret, cstr>)
(defn secret-hkdf-sha256 [^borrow ikm : Secret salt : cstr info : cstr
                          n : int] : result<Secret, cstr>)
```

Notes:

- Every fallible constructor returns `result<Secret, cstr>` -- the
  inline-C result builders (`tur_ok_ptr` / `tur_err_int`, see
  `docs/guides/inline-c-results-guide.md`) make this first-class; no
  `:int` sentinels (see the CLAUDE.md strict rule).
- `with-secret`'s callback type is spelled out as a real `fn` type, not
  `:int`.
- HMAC/HKDF close the loop the existing `stdlib/digest.tur` leaves open
  (raw SHA-256 with no MAC and no constant-time compare invites misuse
  for auth tokens).

### 2.3 Free-standing hygiene primitives (usable without `Secret`)

```turmeric
(defn secure-wipe-ptr!    [p : ptr<void> n : int] : void)
(defn crypto-random-bytes! [p : ptr<void> n : int] : result<int, cstr>)
```

`secure-wipe-ptr!` implementation ladder inside one inline-C body:
`memset_s` (C11 Annex K) -> `explicit_bzero` (BSD/glibc>=2.25) ->
`SecureZeroMemory` (Win32) -> volatile-pointer loop fallback. Never
plain `memset` -- the existing `raw-memset` builtin
(`src/compiler/emit_core.c:3550`) lowers to elidable `memset` and is
NOT a secure wipe; the spice must not reuse it.

`crypto-random-bytes!`: `getrandom(2)` -> `arc4random_buf` (macOS/BSD)
-> `BCryptGenRandom` (Win32) -> mbedTLS CTR-DRBG as the portable
fallback. **This is the highest-value single item in the plan** --
today anyone reaching for `stdlib/random.tur` for a token gets
`rand()`/`srand(time(NULL))`.

### 2.4 Backing library: reuse the tls spice's mbedTLS

The tls spice already pins **mbedTLS 3.6.2** via `:cmake-deps`
(`spices/tls/build.tur`, targets `mbedtls mbedx509 mbedcrypto`, plus a
dedicated `tls/autolink` module carrying
`/* __tur_autolink__: -lmbedtls -lmbedx509 -lmbedcrypto */`). mbedTLS
ships exactly the primitives we would otherwise hand-roll:

- `mbedtls_platform_zeroize` -- vetted secure wipe.
- `mbedtls_ctr_drbg` + `mbedtls_entropy` -- CSPRNG fallback.
- `mbedtls_ct_*` -- constant-time helpers.
- `mbedtls_md_hmac`, `mbedtls_hkdf` -- HMAC/HKDF.

`spices/secret` declares the same `:cmake-deps` row (dedup by
`cmake_name` happens at the generated-CMake layer) and its own
`secret/autolink` module, following the tls pattern verbatim. OS-native
paths (getrandom/arc4random/explicit_bzero) are preferred at runtime;
mbedTLS is the portability floor.

### 2.5 Optional: `secret/do` best-effort scope

A Go-flavored convenience, clearly documented as best-effort:

```turmeric
(defn secret-do [f : (fn [] int)] : int)
```

Runs `f`, then (a) calls a `noinline` C helper that allocates a ~64 KiB
frame and volatile-zeroes it (scrubs the stack region `f` plausibly
used), and (b) clobbers caller-saved registers via inline asm on
x86-64/aarch64. Documentation must state plainly: this is heuristic
stack hygiene, not Go's guarantee -- spills, callee frames deeper than
the scrub window, and compiler-copied temporaries are not covered.
Ship it last; it is the least-defensible piece and the API stands
without it.

### 2.6 Effects row (optional, cheap)

`stdlib/effects.tur` already defines `Rand ^capability ^extends IO`
(`effects.tur:93`). The spice's constructors should carry
`#fx{Rand}` where they draw entropy. A dedicated `Secret` effect row is
NOT proposed -- the linear type already carries the discipline, and the
effects inventory should not grow a row that duplicates what the type
system enforces (discipline is opt-in per `effects.tur:38-40` anyway).

## 3. Interpreter caveat (must be documented, not solved)

None of the memory guarantees hold under `tur --interpret`:

- turi executes inline-C defns only via registered natives or the
  pattern-matching heuristic (`src/turi/eval.c:7727-7776`,
  `try_exec_simple_inline_c` at `eval.c:4377`); none of these bodies
  will match the patterns.
- turi's closures/values are process-lifetime by design; there is no
  point at which "the secret is gone" can be promised.

Resolution: the spice's doc page states guarantees are
**compiled-path only**. If REPL ergonomics matter later, register real
natives for the constructors/consumers (the
`turi_env_register_native_typed` path, `src/turi/eval.h:257`) -- but
even then only the wipe-on-consume behavior transfers, not the
allocation-locking story.

## 4. Why a spice, not stdlib

- Per the standing project gate, stdlib/typeclass growth is justified
  by tur-signal's actual call surface; nothing there consumes key
  material.
- The mbedTLS dependency belongs in spice-land (`:cmake-deps`), keeping
  the compiler's "default configure adds no dependency" policy intact.
- Exception worth making anyway: fix `stdlib/random.tur`'s
  documentation to state it is not crypto-grade and point at the spice
  (one comment block; no behavior change on the v1 track).

## 5. Phases

- **S1 -- hygiene floor.** `secure-wipe-ptr!`, `crypto-random-bytes!`,
  `secret-eq?` (ptr form), spice skeleton + `:cmake-deps` + autolink
  module. Fixture: wipe survives `-O2` (assert the buffer reads back
  zero via a separately-compiled observer TU so the optimizer cannot
  fold the check).
- **S2 -- linear `Secret`.** The opaque type, constructors,
  `secret-wipe!`, `with-secret`, mlock/MADV_DONTDUMP. Fixtures: (a)
  happy path; (b) an `errors/` fixture asserting the TUR-E drop-without-
  consume diagnostic under `-Xsubstructural`; (c) mlock fail-soft.
- **S3 -- crypto ops.** `secret-hmac-sha256`, `secret-hkdf-sha256`,
  constant-time hex/b64 codecs.
- **S4 (optional) -- `secret-do`** best-effort scope, gated on honest
  docs.

Each phase lands with its own tests in the spice's suite; nothing here
touches `tests/run.sh` fixture counts in the compiler repo except the
optional S2 substructural `errors/` fixture if we want compiler-side
coverage of the linear-Secret diagnostic shape.

## 6. Risks / open questions

- **`RLIMIT_MEMLOCK` defaults are small** (64 KiB on many Linux
  distros). Fail-soft is the right default, but `secret-locked?` must
  exist so callers who *require* locking can check.
- **Borrow escape from `with-secret`**: the borrow checker prevents the
  `Secret` escaping, but the raw `ptr<void>` handed to the callback can
  be smuggled out. Document; optionally add a debug-build canary
  (poison the mapping after the call under `TUR_DEBUG_SANITIZE`).
- **macOS has no MADV_DONTDUMP**; nearest analog is minimizing dirty
  pages. Accept the gap, document per-platform coverage in a table.
- **Windows**: `VirtualLock` + `SecureZeroMemory` exist; CSPRNG via
  `BCryptGenRandom`. Defer to whenever the Windows track
  (docs/upcoming/v1/windows-remaining-plan.md) makes spices exercised
  there at all.
