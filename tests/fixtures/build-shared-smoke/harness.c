/* RP0 smoke test harness: dlopen the shared library produced by
 * `tur build --shared`, dlsym the exported defn, call it, and verify
 * the result. Pairs with tests/fixtures/build-shared-smoke/add.tur. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* MinGW ships no <dlfcn.h>; the repo already carries the LoadLibrary shim the
 * compiler itself uses, so reuse it rather than growing a second copy here.
 * tests/run-build-shared.sh puts src/ on the include path for this. */
#ifdef _WIN32
#  include "platform_dl.h"
#else
#  include <dlfcn.h>
#endif

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <lib.so>\n", argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    dlerror();
    typedef int64_t (*add42_fn)(int64_t);
    add42_fn f = (add42_fn)dlsym(h, "smokelib__add42");
    const char *err = dlerror();
    if (!f || err) {
        fprintf(stderr, "dlsym failed: %s\n", err ? err : "(null)");
        dlclose(h);
        return 1;
    }
    int64_t r = f(100);
    if (r != 142) {
        fprintf(stderr, "smokelib__add42(100) = %lld, expected 142\n",
                (long long)r);
        dlclose(h);
        return 1;
    }
    printf("smokelib__add42(100) = %lld\n", (long long)r);
    dlclose(h);
    return 0;
}
