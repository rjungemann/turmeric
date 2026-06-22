# Fix paper trail -- thread-spawn-fn "not removed" (module resolution)

Resolved 2026-06-22. **Not a turmeric defect; the spice applied the fix.**

## Original claim

httpd's `concurrent_test` failed with `unknown function or operator
'thread-spawn-fn'`. The report (correctly) concluded this was NOT a
stdlib rename/removal or compiler API regression: `thread-spawn-fn` is defined
in `stdlib/thread.tur`, which is a T19-C/D library module deliberately NOT in
the compiler's auto-load `stdlib_files[]` set (see `src/main.c`), so the symbol
is only in scope when the program explicitly pulls the module in. The report
could not fully localize because the httpd spice was absent from its container.

## Re-verification (this branch, fresh turmeric-spices clone)

1. `thread-spawn-fn` still defined/exported: `stdlib/thread.tur:49`
   (`defn thread-spawn-fn [fn-ptr : ptr<void> arg : ptr<void>] #{Unsafe} : ThreadHandle`).

2. The report's probe holds verbatim:

   ```
   # without load -> unresolved
   # with (load "stdlib/thread.tur") -> resolves; next diagnostic is the
   #   expected unsafe-context check:
   error: unsafe function 'thread-spawn-fn' requires an enclosing (unsafe ...)
   ```

   The follow-on "unsafe" diagnostic proves name resolution succeeded.

3. `tests/concurrent_test.tur` now begins:

   ```turmeric
   ;; stdlib/thread.tur (thread-spawn-fn / thread-join) is a T19-C/D library
   ;; module, deliberately NOT auto-loaded -- pull it in explicitly ...
   ;; thread-spawn-fn report.
   (load "stdlib/thread.tur")
   ```

   i.e. the spice adopted the report's recommended explicit-load fix; the
   `unknown function 'thread-spawn-fn'` symptom is gone.

## Current concurrent_test status (out of scope here)

`concurrent_test` now fails on a missing vendored `yyjson.h` (the JSON
dependency), reached during elaboration of unrelated decode/json code -- far
past thread-module resolution. That is an httpd spice build-config matter (put
the yyjson header on the include path), independent of this report.

## Turmeric-side outcome

No compiler change. The longer-term `require`/module mechanism noted in the
report (auto-loading the T19-C/D set) remains future work, but the reported
symptom is fully explained and the spice is fixed.
