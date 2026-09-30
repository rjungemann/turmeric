/* fuzz_common.h -- shared scaffolding for the libFuzzer targets in tests/fuzz.
 *
 * security-audit-plan WP4.  Every target here feeds attacker-shaped bytes to
 * one parser that the project promises to handle safely (docs/guides/
 * security-guide.md, boundaries T1, T2 and T5).  See tests/fuzz/README.md for
 * how to build and run them. */
#ifndef TUR_FUZZ_COMMON_H
#define TUR_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A NUL-terminated heap copy of the fuzz input: most of the parsers under
 * test read C strings.  Embedded NULs are kept -- they only end the string
 * early, which is itself a case worth covering. */
static inline char *fuzz_cstr(const uint8_t *data, size_t size) {
    char *s = (char *)malloc(size + 1);
    if (!s) abort();
    if (size) memcpy(s, data, size);
    s[size] = '\0';
    return s;
}

#endif
