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
        case TY_INT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_INT64: case TY_UINT64:
            return 'i';
        /* Exact-width integer classes (scalar-width-fidelity).  The
         * argument direction never needed these -- a narrow int rides the
         * same register and the callee reads the low bits -- but the RETURN
         * direction does: a C callee returning `int` leaves the upper 32
         * bits of the register unspecified, so a thunk reading a 64-bit
         * result out of it hands back garbage for any negative (or
         * high-bit) value.  The thunk's C declaration carries the exact
         * type instead, and the C cast to long long does the correct sign-
         * or zero-extension.  Case matters only for that extension, which
         * is why unsigned gets its own letters here while the aggregate
         * MEMBER codes (layout-only) never needed them. */
        case TY_BOOL:
        case TY_INT8:   return 'b';
        case TY_UINT8:  return 'B';
        case TY_INT16:  return 'h';
        case TY_UINT16: return 'H';
        case TY_INT32:  return 'w';
        case TY_UINT32: return 'W';
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

bool tur_jit_ffi_class_is_int(char c) {
    return c == 'i' || c == 'b' || c == 'B' || c == 'h' || c == 'H' ||
           c == 'w' || c == 'W';
}

char tur_jit_ffi_member_code_for_kind(TypeKind k) {
    switch (k) {
        case TY_BOOL:
        case TY_INT8:  case TY_UINT8:   return 'b';
        case TY_INT16: case TY_UINT16:  return 'h';
        case TY_INT32: case TY_UINT32:  return 'w';
        case TY_INT:
        case TY_INT64: case TY_UINT64:  return 'q';
        case TY_CSTR:
        case TY_PTR_VOID:               return 'p';
        case TY_FLOAT32:                return 'F';
        case TY_FLOAT:
        case TY_FLOAT64:                return 'f';
        default:                        return 0;
    }
}

void tur_jit_ffi_member_size_align(char code, size_t *size, size_t *align) {
    size_t s = 0;
    switch (code) {
        case 'b': s = 1; break;
        case 'h': s = 2; break;
        case 'w': s = 4; break;
        case 'F': s = 4; break;
        case 'q': s = 8; break;
        case 'p': s = 8; break;
        case 'f': s = 8; break;
        default:  s = 0; break;
    }
    /* Every code in this vocabulary is naturally aligned on both supported
     * targets -- alignment equals size.  (The one C type where that is not
     * true, __int128 on aarch64, is deliberately outside the vocabulary; it
     * is also the subject of the fork's 90633091 alignment patch.) */
    if (size)  *size  = s;
    if (align) *align = s;
}

const char *tur_jit_ffi_struct_layout(const char *s, size_t *out_size,
                                      size_t *out_align, size_t *offs,
                                      char *codes, int max, int *out_nleaf) {
    if (!s || *s != '{') return NULL;
    const char *p = s + 1;
    size_t off = 0, align = 1;
    int nleaf = 0, nmemb = 0;

    while (*p && *p != '}') {
        size_t msize, malign;
        if (*p == '{') {
            /* Nested aggregate: its leaves flatten into ours, shifted by the
             * offset the nested aggregate itself lands on. */
            size_t nsize, nalign;
            int sub = 0;
            const char *after = tur_jit_ffi_struct_layout(p, &nsize, &nalign,
                                                          NULL, NULL, 0, &sub);
            if (!after) return NULL;
            msize = nsize; malign = nalign;
            if (malign > align) align = malign;
            off = (off + malign - 1) / malign * malign;
            /* Re-walk the nested aggregate to record its leaves shifted to
             * their final offset.  Cheap (sigs are tiny) and keeps the offset
             * arithmetic in exactly one place. */
            if (offs || codes) {
                size_t sub_offs[64];
                char   sub_codes[64];
                int    sub_n = 0;
                if (!tur_jit_ffi_struct_layout(p, NULL, NULL, sub_offs,
                                               sub_codes, 64, &sub_n))
                    return NULL;
                if (nleaf + sub_n > max) return NULL;
                for (int i = 0; i < sub_n; i++) {
                    if (offs)  offs[nleaf + i]  = off + sub_offs[i];
                    if (codes) codes[nleaf + i] = sub_codes[i];
                }
            }
            nleaf += sub;
            off += msize;
            p = after;
        } else {
            tur_jit_ffi_member_size_align(*p, &msize, &malign);
            if (msize == 0) return NULL;          /* unknown member code */
            if (malign > align) align = malign;
            off = (off + malign - 1) / malign * malign;
            if (offs || codes) {
                if (nleaf >= max) return NULL;
                if (offs)  offs[nleaf]  = off;
                if (codes) codes[nleaf] = *p;
            }
            nleaf++;
            off += msize;
            p++;
        }
        nmemb++;
    }
    if (*p != '}') return NULL;                   /* unbalanced */
    if (nmemb == 0) return NULL;                  /* empty aggregate */

    if (out_size)  *out_size  = (off + align - 1) / align * align;
    if (out_align) *out_align = align;
    if (out_nleaf) *out_nleaf = nleaf;
    return p + 1;
}

/* The aarch64 HFA refusal that used to guard this -- and the
 * tur_jit_ffi_is_hfa predicate that existed only to drive it -- are GONE.  The
 * pinned MIR fork now classifies AAPCS64 Homogeneous Floating-point Aggregates
 * and passes/returns them in v0..v7, so a c2mir thunk and a natively compiled
 * callee agree and there is nothing left to refuse.  See
 * docs/archive/mir-aarch64-fp-aggregate-abi.md, and note that reverting the
 * MIR pin below that fix silently reinstates the miscall this file used to
 * catch. */
bool tur_jit_ffi_struct_supported(const char *s, const char **why) {
    if (!tur_jit_ffi_struct_layout(s, NULL, NULL, NULL, NULL, 0, NULL)) {
        if (why) *why = "malformed aggregate signature";
        return false;
    }
    (void)s;
    return true;
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
