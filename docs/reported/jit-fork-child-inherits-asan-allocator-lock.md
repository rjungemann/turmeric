# A child forked under the sanitized `tur jit` can hang on ASan's allocator lock

**Severity: low (CI flake on the JIT legs; no product impact).** Only the Debug
(`-fsanitize=address`) `tur jit` is affected, because only there does the
program run on ASan's allocator: the compiled build and a Release `tur jit`
use glibc's `malloc`, which is fork-safe. The Debug + JIT configuration is
exactly what CI's `JIT engine` legs run, so there it shows up as an
intermittent failure.

Found 2026-09-29 while validating the JIT prune
([jit-suite-pays-for-the-whole-prelude](../archive/jit-suite-pays-for-the-whole-prelude.md));
it reproduces identically with `TUR_JIT_NO_PRUNE=1`, so it is not caused by
that change.

## Repro

```sh
for i in 1 2 3 4 5 6; do
  ./build-jit/tur jit tests/fixtures/r7rs-threads-lifecycle/input.tur 2>/dev/null |
    grep fork-failures
done
# (fork-failures 0) most runs; (fork-failures 1) about 1 run in 3 locally
```

`build-jit` is the CI configuration: `-DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON`
(GCC 13.3's libsanitizer here). The failing child dies of its own `alarm(5)`
(`WIFSIGNALED`, `SIGALRM`) at a random iteration of the 60 forks.

## Root cause

gdb on a hung child (the fork check in
`tests/fixtures/r7rs-threads-lifecycle/life.tur`, `life-forks`):

```
#0  __sanitizer::FutexWait
#2  __sanitizer::Mutex::Lock
#4  SizeClassAllocator64<__asan::AP64<...>>::GetFromAllocator (class_id=3)
#8  __asan::Allocator::Allocate
#10 ___interceptor_malloc (size=32)
#11 tur_region_alloc_or_malloc (n=32)   src/runtime/region.c:334
#12 ?? (JIT-compiled Scheme code: the child's first allocation)
```

The child blocks on ASan's per-size-class region mutex. The burner thread held
it, mid-`malloc`, at the moment of the fork, and a forked child inherits a held
mutex with no thread to release it. glibc's `malloc` takes its arena locks in
its own fork handlers for exactly this reason. This libsanitizer's allocator
does not, so any child of a multithreaded sanitized process that allocates can
hang.

This is the same shape as the archived
[jit-fork-child-hangs-with-threads](../archive/jit-fork-child-hangs-with-threads.md)
(the JIT's `g_gen_lock`, fixed with `pthread_atfork` in `src/jit_engine.c`).
That fix is in place and holds. This lock is ASan's, which `tur` cannot take.

## Fix directions

- **In the fixture:** skip the fork check when the program runs on a sanitizer
  allocator. That means under `TUR_JIT_ENGINE` in a `__SANITIZE_ADDRESS__` /
  `__has_feature(address_sanitizer)` host. It was skipped under
  `TUR_JIT_ENGINE` before the `g_gen_lock` fix, so this narrows that skip
  rather than restoring it. The inline-C body sees the program's macros, not
  the host's, so the host would need to pass the fact in (e.g. a
  `TUR_JIT_HOST_ASAN` define in `JIT_PRELUDE`).
- **In the host:** recent compiler-rt locks the allocator around `fork` itself
  (an `InstallAtForkHandler` hook; not verified here which LLVM release, or
  which GCC libsanitizer merge, first carries it). A toolchain whose ASan
  runtime does that closes this with no change in the repo. Check the runner's
  toolchain before relying on it.
- Not a fix: raising the child's `alarm` -- the lock is never released.
