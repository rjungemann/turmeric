/* r7rs-lang-plan T1 -- the exact integers outside int64 of `#lang r7rs`:
 * ONE arithmetic core for every place a bignum is made or used.  The same text
 * is
 *   src/compiler/r7rs_bignum.inc  #included by the source reader (reader.c,
 *                                 through r7rs_numsyntax.inc, which reads a
 *                                 long literal) and the interpreter's natives
 *   stdlib/r7rs/bignum.tur        the file-scope C block the prelude's
 *                                 arithmetic calls on the compiled back end
 * and tests/check-r7rs-inc-sync.sh checks the copies agree.  Edit this file,
 * then run tools/gen-r7rs-inc.py to copy it into the .tur block.
 *
 * A bignum crosses every boundary as its canonical DECIMAL spelling (`-` and
 * digits, no leading zeros): that is what the prelude's `R7rsBig` holds, what
 * `write` prints, and what the interpreter passes as a string.  Inside, a
 * number is base-1e9 limbs, least significant first, so decimal in and out
 * is a regrouping of nine digits, not a conversion.  Every entry point takes
 * and returns decimal strings (a result is malloc'd and owned by the caller);
 * the int64 fast path and the choice between an int and an `R7rsBig` are the
 * prelude's.  Functions are `static inline` so a file that includes this and
 * uses only part of it (the reader) compiles without unused warnings. */

#define R7BN_BASE 1000000000u

typedef struct {
    int neg;           /* 1 when negative; zero is never negative */
    size_t n, cap;     /* limbs in use, limbs allocated */
    uint32_t *d;       /* base-1e9 limbs, least significant first */
} r7rs_bn;

static inline void r7bn_init(r7rs_bn *a) { a->neg = 0; a->n = 0; a->cap = 0; a->d = NULL; }
static inline void r7bn_free(r7rs_bn *a) { free(a->d); r7bn_init(a); }
static inline void r7bn_reserve(r7rs_bn *a, size_t n) {
    if (n <= a->cap) return;
    size_t cap = a->cap ? a->cap : 4;
    while (cap < n) cap *= 2;
    uint32_t *d = (uint32_t *)realloc(a->d, cap * sizeof *d);
    if (!d) abort();
    a->d = d; a->cap = cap;
}
static inline void r7bn_trim(r7rs_bn *a) {
    while (a->n && a->d[a->n - 1] == 0) a->n--;
    if (a->n == 0) a->neg = 0;
}
static inline void r7bn_copy(r7rs_bn *r, const r7rs_bn *a) {
    r7bn_reserve(r, a->n);
    if (a->n) memcpy(r->d, a->d, a->n * sizeof *a->d);
    r->n = a->n; r->neg = a->neg;
}
static inline void r7bn_from_u64(r7rs_bn *a, uint64_t v, int neg) {
    r7bn_reserve(a, 3);
    a->n = 0;
    while (v) { a->d[a->n++] = (uint32_t)(v % R7BN_BASE); v /= R7BN_BASE; }
    a->neg = neg;
    r7bn_trim(a);
}
static inline void r7bn_from_i64(r7rs_bn *a, int64_t v) {
    r7bn_from_u64(a, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, v < 0);
}
/* |a| = |a| * m + add, for m and add below the base. */
static inline void r7bn_mul_small_add(r7rs_bn *a, uint32_t m, uint32_t add) {
    uint64_t carry = add;
    for (size_t i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)(t % R7BN_BASE);
        carry = t / R7BN_BASE;
    }
    while (carry) { r7bn_reserve(a, a->n + 1); a->d[a->n++] = (uint32_t)(carry % R7BN_BASE); carry /= R7BN_BASE; }
    r7bn_trim(a);
}
/* |a| = |a| / m; the remainder is returned.  m nonzero, below the base. */
static inline uint32_t r7bn_divmod_small(r7rs_bn *a, uint32_t m) {
    uint64_t rem = 0;
    for (size_t i = a->n; i-- > 0;) {
        uint64_t cur = a->d[i] + rem * R7BN_BASE;
        a->d[i] = (uint32_t)(cur / m);
        rem = cur % m;
    }
    r7bn_trim(a);
    return (uint32_t)rem;
}
/* The magnitude spelled by digits s[0..n) in radix 2..36 (every byte a valid
 * digit; the caller checked). */
static inline void r7bn_from_digits(r7rs_bn *a, const char *s, size_t n, int radix, int neg) {
    uint32_t chunk_max = 1; int per = 0;
    while ((uint64_t)chunk_max * (uint64_t)radix <= R7BN_BASE) { chunk_max *= (uint32_t)radix; per++; }
    a->n = 0; a->neg = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t chunk = 0, mul = 1;
        for (int k = 0; k < per && i < n; k++, i++) {
            int c = (unsigned char)s[i];
            int v = (c >= '0' && c <= '9') ? c - '0' : (c | 32) - 'a' + 10;
            chunk = chunk * (uint32_t)radix + (uint32_t)v;
            mul *= (uint32_t)radix;
        }
        if (a->n == 0) { if (chunk) { r7bn_reserve(a, 1); a->d[0] = chunk; a->n = 1; } }
        else r7bn_mul_small_add(a, mul, chunk);
    }
    a->neg = neg;
    r7bn_trim(a);
}
/* A canonical decimal spelling ("-123"); 0 when s is not one. */
static inline int r7bn_from_dec(r7rs_bn *a, const char *s) {
    int neg = 0;
    if (*s == '-' || *s == '+') { neg = (*s == '-'); s++; }
    size_t n = strlen(s);
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    r7bn_from_digits(a, s, n, 10, neg);
    return 1;
}
/* The spelling of a in radix 2..36, malloc'd. */
static inline char *r7bn_to_str(const r7rs_bn *a, int radix) {
    if (a->n == 0) { char *z = (char *)malloc(2); if (!z) abort(); z[0] = '0'; z[1] = 0; return z; }
    if (radix == 10) {
        char *r = (char *)malloc(a->n * 9 + 2);
        if (!r) abort();
        char *o = r;
        if (a->neg) *o++ = '-';
        char top[10]; int tn = 0;
        for (uint32_t v = a->d[a->n - 1]; v; v /= 10) top[tn++] = (char)('0' + v % 10);
        while (tn) *o++ = top[--tn];
        for (size_t i = a->n - 1; i-- > 0;) {
            uint32_t v = a->d[i];
            for (int k = 8; k >= 0; k--) { o[k] = (char)('0' + v % 10); v /= 10; }
            o += 9;
        }
        *o = 0;
        return r;
    }
    r7rs_bn t; r7bn_init(&t); r7bn_copy(&t, a);
    size_t cap = a->n * 30 + 3, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) abort();
    while (t.n) {
        uint32_t d = r7bn_divmod_small(&t, (uint32_t)radix);
        buf[len++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    if (a->neg) buf[len++] = '-';
    for (size_t i = 0; i < len / 2; i++) { char c = buf[i]; buf[i] = buf[len - 1 - i]; buf[len - 1 - i] = c; }
    buf[len] = 0;
    r7bn_free(&t);
    return buf;
}
static inline int r7bn_cmp_mag(const r7rs_bn *a, const r7rs_bn *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (size_t i = a->n; i-- > 0;)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static inline int r7bn_cmp(const r7rs_bn *a, const r7rs_bn *b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c = r7bn_cmp_mag(a, b);
    return a->neg ? -c : c;
}
/* r = |a| + |b|  /  r = |a| - |b| (|a| >= |b|); r may alias neither. */
static inline void r7bn_add_mag(r7rs_bn *r, const r7rs_bn *a, const r7rs_bn *b) {
    size_t n = a->n > b->n ? a->n : b->n;
    r7bn_reserve(r, n + 1);
    uint32_t carry = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t t = (i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0) + carry;
        carry = t >= R7BN_BASE;
        r->d[i] = carry ? t - R7BN_BASE : t;
    }
    r->d[n] = carry;
    r->n = n + 1;
    r7bn_trim(r);
}
static inline void r7bn_sub_mag(r7rs_bn *r, const r7rs_bn *a, const r7rs_bn *b) {
    r7bn_reserve(r, a->n);
    int64_t borrow = 0;
    for (size_t i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        borrow = t < 0;
        r->d[i] = (uint32_t)(borrow ? t + R7BN_BASE : t);
    }
    r->n = a->n;
    r7bn_trim(r);
}
/* r = a + b (bneg flips b's sign: r = a - b). */
static inline void r7bn_addsub(r7rs_bn *r, const r7rs_bn *a, const r7rs_bn *b, int bneg) {
    int bs = b->n ? (b->neg ^ bneg) : 0;
    if (a->neg == bs) { r7bn_add_mag(r, a, b); r->neg = r->n ? a->neg : 0; return; }
    if (r7bn_cmp_mag(a, b) >= 0) { r7bn_sub_mag(r, a, b); r->neg = r->n ? a->neg : 0; }
    else { r7bn_sub_mag(r, b, a); r->neg = r->n ? bs : 0; }
}
static inline void r7bn_mul(r7rs_bn *r, const r7rs_bn *a, const r7rs_bn *b) {
    if (a->n == 0 || b->n == 0) { r->n = 0; r->neg = 0; return; }
    size_t n = a->n + b->n;
    uint64_t *acc = (uint64_t *)calloc(n, sizeof *acc);
    if (!acc) abort();
    for (size_t i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b->n; j++) {
            uint64_t t = acc[i + j] + (uint64_t)a->d[i] * b->d[j] + carry;
            acc[i + j] = t % R7BN_BASE;
            carry = t / R7BN_BASE;
        }
        size_t k = i + b->n;
        while (carry) { uint64_t t = acc[k] + carry; acc[k] = t % R7BN_BASE; carry = t / R7BN_BASE; k++; }
    }
    r7bn_reserve(r, n);
    for (size_t i = 0; i < n; i++) r->d[i] = (uint32_t)acc[i];
    free(acc);
    r->n = n;
    r->neg = a->neg ^ b->neg;
    r7bn_trim(r);
}
/* Truncating division: q = trunc(a / b), m = a - q*b (m takes a's sign).
 * b nonzero; q and m alias nothing.  Knuth's algorithm D in base 1e9. */
static inline void r7bn_divmod(r7rs_bn *q, r7rs_bn *m, const r7rs_bn *a, const r7rs_bn *b) {
    int qneg = a->neg ^ b->neg, mneg = a->neg;
    if (r7bn_cmp_mag(a, b) < 0) {
        r7bn_copy(m, a); m->neg = m->n ? mneg : 0;
        q->n = 0; q->neg = 0;
        return;
    }
    if (b->n == 1) {
        r7bn_copy(q, a);
        uint32_t rem = r7bn_divmod_small(q, b->d[0]);
        q->neg = q->n ? qneg : 0;
        r7bn_from_u64(m, rem, mneg);
        return;
    }
    size_t n = b->n, mlen = a->n - b->n;
    uint32_t f = R7BN_BASE / (b->d[n - 1] + 1);
    r7rs_bn u, v; r7bn_init(&u); r7bn_init(&v);
    r7bn_copy(&u, a); u.neg = 0; r7bn_copy(&v, b); v.neg = 0;
    r7bn_mul_small_add(&u, f, 0);
    r7bn_mul_small_add(&v, f, 0);
    r7bn_reserve(&u, a->n + 1);
    for (size_t i = u.n; i < a->n + 1; i++) u.d[i] = 0;
    r7bn_reserve(q, mlen + 1);
    for (size_t j = mlen + 1; j-- > 0;) {
        uint64_t num = (uint64_t)u.d[j + n] * R7BN_BASE + u.d[j + n - 1];
        uint64_t qhat = num / v.d[n - 1], rhat = num % v.d[n - 1];
        while (qhat >= R7BN_BASE || qhat * v.d[n - 2] > rhat * R7BN_BASE + u.d[j + n - 2]) {
            qhat--; rhat += v.d[n - 1];
            if (rhat >= R7BN_BASE) break;
        }
        int64_t borrow = 0; uint64_t carry = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t p = qhat * v.d[i] + carry;
            carry = p / R7BN_BASE;
            int64_t t = (int64_t)u.d[i + j] - (int64_t)(p % R7BN_BASE) - borrow;
            borrow = t < 0;
            u.d[i + j] = (uint32_t)(borrow ? t + R7BN_BASE : t);
        }
        int64_t t = (int64_t)u.d[j + n] - (int64_t)carry - borrow;
        borrow = t < 0;
        u.d[j + n] = (uint32_t)(borrow ? t + R7BN_BASE : t);
        if (borrow) {
            qhat--;
            uint32_t c = 0;
            for (size_t i = 0; i < n; i++) {
                uint32_t s2 = u.d[i + j] + v.d[i] + c;
                c = s2 >= R7BN_BASE;
                u.d[i + j] = c ? s2 - R7BN_BASE : s2;
            }
            u.d[j + n] = (uint32_t)((u.d[j + n] + c) % R7BN_BASE);
        }
        q->d[j] = (uint32_t)qhat;
    }
    q->n = mlen + 1; q->neg = qneg; r7bn_trim(q);
    u.n = n; r7bn_trim(&u);
    (void)r7bn_divmod_small(&u, f);
    r7bn_copy(m, &u); m->neg = m->n ? mneg : 0;
    r7bn_free(&u); r7bn_free(&v);
}
/* a as an int64 when it fits. */
static inline int r7bn_to_i64(const r7rs_bn *a, int64_t *out) {
    if (a->n > 3) return 0;
    uint64_t m = 0;
    for (size_t i = a->n; i-- > 0;) {
        if (m > (UINT64_MAX - a->d[i]) / R7BN_BASE) return 0;
        m = m * R7BN_BASE + a->d[i];
    }
    if (a->neg ? m > (uint64_t)INT64_MAX + 1u : m > (uint64_t)INT64_MAX) return 0;
    *out = a->neg ? (int64_t)(~m + 1u) : (int64_t)m;
    return 1;
}
/* The nearest double (correctly rounded, through strtod). */
static inline double r7bn_to_double(const r7rs_bn *a) {
    char *s = r7bn_to_str(a, 10);
    double f = strtod(s, NULL);
    free(s);
    return f;
}
/* A finite, integral double as a bignum, exactly. */
static inline void r7bn_from_double(r7rs_bn *a, double f) {
    uint64_t bits; memcpy(&bits, &f, sizeof bits);
    int neg = (int)(bits >> 63), e = (int)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & ((1ull << 52) - 1);
    if (e == 0) { a->n = 0; a->neg = 0; return; }          /* zero or subnormal: |f| < 1 */
    mant |= 1ull << 52;
    int shift = e - 1075;
    if (shift <= 0) { r7bn_from_u64(a, shift <= -64 ? 0 : mant >> -shift, neg); return; }
    r7bn_from_u64(a, mant, 0);
    while (shift > 0) { int s = shift > 29 ? 29 : shift; r7bn_mul_small_add(a, 1u << s, 0); shift -= s; }
    a->neg = a->n ? neg : 0;
}
/* a against a double, exactly: -1, 0 or 1, or 2 when f is a NaN. */
static inline int r7bn_cmp_double(const r7rs_bn *a, double f) {
    if (f != f) return 2;
    if (f == __builtin_inf()) return -1;
    if (f == -__builtin_inf()) return 1;
    double t = __builtin_trunc(f);
    r7rs_bn b; r7bn_init(&b); r7bn_from_double(&b, t);
    int c = r7bn_cmp(a, &b);
    r7bn_free(&b);
    if (c) return c;
    return f > t ? -1 : f < t ? 1 : 0;
}
/* s = floor(sqrt(a)), a >= 0: Newton's iteration from above. */
static inline void r7bn_isqrt(r7rs_bn *s, const r7rs_bn *a) {
    if (a->n == 0) { s->n = 0; s->neg = 0; return; }
    r7rs_bn x, y, q, m, t; r7bn_init(&x); r7bn_init(&y); r7bn_init(&q); r7bn_init(&m); r7bn_init(&t);
    size_t k = (a->n + 1) / 2;                 /* x = base^k >= sqrt(a) */
    r7bn_reserve(&x, k + 1);
    for (size_t i = 0; i < k; i++) x.d[i] = 0;
    x.d[k] = 1; x.n = k + 1;
    for (;;) {
        r7bn_divmod(&q, &m, a, &x);
        r7bn_add_mag(&t, &x, &q);
        (void)r7bn_divmod_small(&t, 2);
        if (r7bn_cmp_mag(&t, &x) >= 0) break;
        r7bn_copy(&x, &t);
    }
    r7bn_copy(s, &x);
    r7bn_free(&x); r7bn_free(&y); r7bn_free(&q); r7bn_free(&m); r7bn_free(&t);
}

/* ---- the entry points: decimal strings in, a malloc'd decimal string out */

enum { R7BN_ADD = 0, R7BN_SUB = 1, R7BN_MUL = 2, R7BN_QUO = 3, R7BN_REM = 4,
       R7BN_MOD = 5, R7BN_GCD = 6, R7BN_EXPT = 7, R7BN_ISQRT = 8, R7BN_NEG = 9 };

/* r7rs_big_op -- one operation on two exact integers.  QUO/REM truncate, MOD
 * floors; the caller has refused a zero divisor.  EXPT takes b >= 0; ISQRT
 * and NEG ignore b. */
static inline char *r7rs_big_op(int op, const char *as, const char *bs) {
    r7rs_bn a, b, r, m;
    r7bn_init(&a); r7bn_init(&b); r7bn_init(&r); r7bn_init(&m);
    (void)r7bn_from_dec(&a, as);
    (void)r7bn_from_dec(&b, bs ? bs : "0");
    switch (op) {
        case R7BN_ADD: r7bn_addsub(&r, &a, &b, 0); break;
        case R7BN_SUB: r7bn_addsub(&r, &a, &b, 1); break;
        case R7BN_MUL: r7bn_mul(&r, &a, &b); break;
        case R7BN_QUO: r7bn_divmod(&r, &m, &a, &b); break;
        case R7BN_REM: r7bn_divmod(&m, &r, &a, &b); break;
        case R7BN_MOD:
            r7bn_divmod(&m, &r, &a, &b);
            if (r.n && r.neg != b.neg) { r7rs_bn t; r7bn_init(&t); r7bn_addsub(&t, &r, &b, 0); r7bn_copy(&r, &t); r7bn_free(&t); }
            break;
        case R7BN_GCD: {
            a.neg = 0; b.neg = 0;
            while (b.n) { r7bn_divmod(&m, &r, &a, &b); r7bn_copy(&a, &b); r7bn_copy(&b, &r); }
            r7bn_copy(&r, &a);
            break;
        }
        case R7BN_EXPT: {
            r7bn_from_u64(&r, 1, 0);
            while (b.n) {
                if (b.d[0] & 1) { r7bn_mul(&m, &r, &a); r7bn_copy(&r, &m); }
                (void)r7bn_divmod_small(&b, 2);
                if (b.n) { r7bn_mul(&m, &a, &a); r7bn_copy(&a, &m); }
            }
            break;
        }
        case R7BN_ISQRT: r7bn_isqrt(&r, &a); break;
        case R7BN_NEG: r7bn_copy(&r, &a); r.neg = r.n ? !a.neg : 0; break;
        default: break;
    }
    char *s = r7bn_to_str(&r, 10);
    r7bn_free(&a); r7bn_free(&b); r7bn_free(&r); r7bn_free(&m);
    return s;
}
/* r7rs_big_cmp -- -1, 0 or 1. */
static inline int64_t r7rs_big_cmp(const char *as, const char *bs) {
    r7rs_bn a, b; r7bn_init(&a); r7bn_init(&b);
    (void)r7bn_from_dec(&a, as); (void)r7bn_from_dec(&b, bs);
    int c = r7bn_cmp(&a, &b);
    r7bn_free(&a); r7bn_free(&b);
    return c;
}
/* r7rs_big_cmp_float -- the exact integer against a double, exactly: -1, 0
 * or 1, or 2 when the double is a NaN (every comparison false). */
static inline int64_t r7rs_big_cmp_float(const char *as, double f) {
    r7rs_bn a; r7bn_init(&a); (void)r7bn_from_dec(&a, as);
    int c = r7bn_cmp_double(&a, f);
    r7bn_free(&a);
    return c;
}
static inline double r7rs_big_to_float(const char *as) { return strtod(as, NULL); }
/* r7rs_big_of_float -- a finite integral double, exactly. */
static inline char *r7rs_big_of_float(double f) {
    r7rs_bn a; r7bn_init(&a); r7bn_from_double(&a, f);
    char *s = r7bn_to_str(&a, 10);
    r7bn_free(&a);
    return s;
}
/* r7rs_big_radix -- the spelling in radix 2, 8, 10 or 16 (number->string). */
static inline char *r7rs_big_radix(const char *as, int radix) {
    r7rs_bn a; r7bn_init(&a); (void)r7bn_from_dec(&a, as);
    char *s = r7bn_to_str(&a, radix);
    r7bn_free(&a);
    return s;
}
/* r7rs_big_fits -- the decimal spelling is an int64 (then *out holds it). */
static inline int r7rs_big_fits(const char *as, int64_t *out) {
    r7rs_bn a; r7bn_init(&a);
    int ok = r7bn_from_dec(&a, as) && r7bn_to_i64(&a, out);
    r7bn_free(&a);
    return ok;
}
