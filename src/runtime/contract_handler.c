/* CT4: the contract-violation handler registry -- HOST-resident runtime API.
 *
 * Emitted programs call tur_set_contract_handler / tur_get_contract_handler
 * (via stdlib/contract.tur's extern-c surface) but never define them: the
 * pair resolves by address into the host process, exactly like hamt.c.  It
 * used to live in src/runtime/runtime.c; when S2 replaced that TU's panic/
 * continuation families with the generated runtime TU
 * (src/runtime/generated/tur_rt_split.c), this pair -- which is NOT part of
 * the emitted preamble and so not in the generated TU -- needed its own
 * home.  The first post-swap JIT sweep failed 1,656 fixtures on exactly
 * `import of undefined item tur_set_contract_handler`, which is this file's
 * reason to exist.
 *
 * Default handler is NULL: tur-contract-check forwards to tur_panic when no
 * override is registered. */

#include <stddef.h>

static void (*g_contract_handler)(const char *) = NULL;

void tur_set_contract_handler(void (*h)(const char *)) {
    g_contract_handler = h;
}

/* ISO C forbids converting a function pointer to void *, but every supported
 * platform (POSIX) guarantees the round-trip; the handler is round-tripped
 * back through tur_set_contract_handler. Silence -Wpedantic for this case. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
void *tur_get_contract_handler(void) {
    return (void *)g_contract_handler;
}
#pragma GCC diagnostic pop
