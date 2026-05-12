/* platform.h - Platform-specific macros for Turmeric (Phase T19)
 *
 * Centralises compiler/platform portability macros so that the rest of the
 * codebase never needs raw #ifdef chains.
 *
 * TUR_THREAD_LOCAL
 *   Thread-local-storage class specifier.  Expands to __thread (GNU extension,
 *   available in Apple Clang and GCC on all supported targets).  _Thread_local
 *   is the C11 standard spelling but is not available on older Apple SDKs;
 *   __thread is the safe portable choice for the supported platforms (macOS
 *   arm64/x86-64, Linux x86-64/arm64).
 *
 * TUR_UNUSED(x)
 *   Silence unused-variable warnings for intentionally unused parameters.
 *
 * TUR_LIKELY / TUR_UNLIKELY
 *   Branch-prediction hints.
 */

#ifndef TUR_PLATFORM_H
#define TUR_PLATFORM_H

/* ── Thread-local storage ─────────────────────────────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
#  define TUR_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
#  define TUR_THREAD_LOCAL __declspec(thread)
#else
/* C11 fallback — requires <threads.h> which is absent on some platforms */
#  define TUR_THREAD_LOCAL _Thread_local
#endif

/* ── Utility macros ───────────────────────────────────────────────────────── */
#define TUR_UNUSED(x) ((void)(x))

#if defined(__GNUC__) || defined(__clang__)
#  define TUR_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define TUR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define TUR_LIKELY(x)   (x)
#  define TUR_UNLIKELY(x) (x)
#endif

#endif /* TUR_PLATFORM_H */
