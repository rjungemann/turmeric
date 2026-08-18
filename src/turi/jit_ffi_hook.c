/* jit_ffi_hook.c -- the tur_core half of the JIT-scoped dynamic FFI
 * (jit-ffi-c2mir-plan).  Holds the provider pointer and the signature
 * vocabulary helpers; contains no MIR code, so every tur_core consumer
 * (WASM build included) links it unchanged.  The provider itself lives in
 * jit_ffi.c inside tur_jit_obj and is installed from main() in JIT builds
 * only -- same split as the TurSpiceJitHook. */
#include "jit_ffi.h"

#include <string.h>

static const TurJitFfiProvider *g_jit_ffi_provider = NULL;

void tur_jit_ffi_set_provider(const TurJitFfiProvider *p) {
    g_jit_ffi_provider = p;
}

const TurJitFfiProvider *tur_jit_ffi_provider(void) {
    return g_jit_ffi_provider;
}

char tur_jit_ffi_class_for_kind(TypeKind k, int is_return) {
    switch (k) {
        case TY_NIL:      return is_return ? 'v' : '?';
        case TY_BOOL:
        case TY_INT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_INT8:  case TY_INT16:  case TY_INT32:  case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            return 'i';
        case TY_FLOAT:
        case TY_FLOAT64:
            return 'f';
        case TY_FLOAT32:
            /* Exact ABI class: a float32 crosses the boundary as float, not
             * widened to double -- the thunk casts fv[k] back down.  This is
             * the class codegen's shim mapping lacks ('f' there), which is
             * fine: the shim bakes the concrete C parameter type in, while a
             * runtime thunk has only this string to go on. */
            return 'F';
        default:
            return '?';
    }
}

int tur_jit_ffi_sig_render(TypeKind ret, const TypeKind *params, uint32_t n,
                           char *buf, size_t bufcap) {
    if (!buf || bufcap < (size_t)n + 3) return -1;
    char rc = tur_jit_ffi_class_for_kind(ret, 1);
    if (rc == '?') return -1;
    size_t pos = 0;
    buf[pos++] = rc;
    buf[pos++] = ':';
    for (uint32_t i = 0; i < n; i++) {
        char c = tur_jit_ffi_class_for_kind(params ? params[i] : TY_NIL, 0);
        if (c == '?' || c == 'v') return -1;
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return 0;
}
