/* r7rs-lang-plan T0 -- the R7RS number syntax (7.1.1 <number>): ONE parser for
 * every place a `#lang r7rs` number is read.  The same text is
 *   src/compiler/r7rs_numsyntax.inc  #included by the source reader (reader.c)
 *                                    and the interpreter's natives
 *   stdlib/r7rs/numsyntax.tur        the file-scope C block `string->number`
 *                                    and `read` call on the compiled back end
 * and tests/check-r7rs-inc-sync.sh checks the copies agree.  Edit this file,
 * then run tools/gen-r7rs-inc.py to copy it into the .tur block.  It needs
 * r7rs_bignum.inc included first (T1), for literals outside int64.
 *
 * It reads the whole grammar -- prefixes, ratios, decimals with the R5RS
 * exponent markers, infinities, rectangular and polar complex numbers -- so
 * a token is never split into a number and a stray symbol.  An exact integer
 * outside int64 is a bignum (T1, R7NS_BIG) and an exact non-integer a ratio
 * (T2, R7NS_RATIO), each spelled in a malloc'd string the caller frees; a
 * ratio that divides is the integer (`10/2` is 5).  A complex number whose
 * imaginary part is an exact zero is the real it is (`3+0i` is 3, R7RS
 * 6.2.6); any other is R7NS_COMPLEX (T6), its two parts spelled as reals in
 * `big` ("RE IM"), which the same parser reads back in radix 10.  A polar
 * number `m@a` is converted to its rectangular parts, inexact.  What is not
 * a number (a zero denominator, `#e+inf.0`) is REFUSED with the reason, never
 * read as something else. */
#include <math.h>

enum { R7NS_NONE = 0, R7NS_INT = 1, R7NS_FLOAT = 2, R7NS_REFUSED = 3, R7NS_BIG = 4,
       R7NS_RATIO = 5, R7NS_COMPLEX = 6 };

typedef struct {
    int kind;          /* R7NS_*: not a number, an exact int, a double,
                        * refused, an exact integer outside int64, an exact
                        * non-integer */
    int64_t i;         /* R7NS_INT */
    double f;          /* R7NS_FLOAT */
    const char *why;   /* R7NS_REFUSED: why, as a sentence fragment */
    char *big;         /* R7NS_BIG: decimal digits; R7NS_RATIO: "n/d" in
                        * lowest terms, the sign on n; R7NS_COMPLEX: "RE IM",
                        * each part a real's spelling.  malloc'd, the caller's */
} r7rs_ns_result;

static const char r7rs_ns_why_bigexp[] =
    "the exponent is too large for an exact number";
static const char r7rs_ns_why_div0[] =
    "an exact rational with a zero denominator is not a number";
static const char r7rs_ns_why_infnan[] =
    "an infinity or NaN has no exact representation";

/* One real as read.  Exact: sign and the magnitudes n/d (`big` when one
 * left 64 bits; the digit text -- s[ns..ne) over s[ds..de) in `radix` -- is
 * what the bignum core reads).  Inexact: the double, and for a decimal
 * lexeme (`dec`) its text s[ns..ne), which `#e` reads exactly (T2: `#e1.2`
 * is 6/5, not the double's value). */
typedef struct {
    int exact, neg, big, radix, dec;
    uint64_t n, d;
    double f;
    const char *s;
    size_t ns, ne, ds, de;
} r7rs_ns_real;

static int r7rs_ns_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int r7rs_ns_digit(int c, int radix) {
    int v = (c >= '0' && c <= '9') ? c - '0'
          : (c >= 'a' && c <= 'z') ? c - 'a' + 10
          : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99;
    return v < radix ? v : -1;
}

/* `+inf.0` / `-inf.0` / `+nan.0` / `-nan.0`, any case, at s[p]. */
static int r7rs_ns_infnan(const char *s, size_t n, size_t p, double *out) {
    if (p + 6 > n || (s[p] != '+' && s[p] != '-')) return 0;
    char w[6];
    for (int k = 0; k < 5; k++) w[k] = (char)r7rs_ns_lower((unsigned char)s[p + 1 + k]);
    w[5] = 0;
    if (!strcmp(w, "inf.0")) { *out = (s[p] == '-') ? -__builtin_inf() : __builtin_inf(); return 1; }
    if (!strcmp(w, "nan.0")) { *out = __builtin_nan(""); return 1; }
    return 0;
}

/* <ureal R> at s[*p]: a uinteger, a ratio, or (radix 10) a decimal with an
 * optional exponent -- `e`, or R5RS's `s` `f` `d` `l`, which mean `e`. */
static int r7rs_ns_ureal(const char *s, size_t n, size_t *p, int radix, r7rs_ns_real *x) {
    size_t q = *p, d0 = q;
    while (q < n && r7rs_ns_digit((unsigned char)s[q], radix) >= 0) q++;
    size_t nd = q - d0;
    if (radix == 10) {
        int dec = 0;
        if (q < n && s[q] == '.') {
            dec = 1; q++;
            size_t f0 = q;
            while (q < n && s[q] >= '0' && s[q] <= '9') q++;
            nd += q - f0;
            if (nd == 0) return 0;
        }
        if (nd > 0 && q < n && s[q] && strchr("esfdl", r7rs_ns_lower((unsigned char)s[q]))) {
            size_t e = q + 1;
            if (e < n && (s[e] == '+' || s[e] == '-')) e++;
            if (e < n && s[e] >= '0' && s[e] <= '9') {
                while (e < n && s[e] >= '0' && s[e] <= '9') e++;
                dec = 1; q = e;
            }
        }
        if (dec) {
            char small[64], *buf = small;
            size_t len = q - d0;
            if (len + 1 > sizeof small) { buf = (char *)malloc(len + 1); if (!buf) return 0; }
            for (size_t k = 0; k < len; k++) {
                char c = s[d0 + k];
                buf[k] = (char)(strchr("sSfFdDlL", c) && c ? 'e' : c);
            }
            buf[len] = 0;
            x->exact = 0; x->f = strtod(buf, NULL);
            x->dec = 1; x->s = s; x->ns = d0; x->ne = q;
            if (buf != small) free(buf);
            *p = q;
            return 1;
        }
    }
    if (nd == 0) return 0;
    uint64_t m = 0; int big = 0;
    for (size_t k = d0; k < q; k++) {
        int v = r7rs_ns_digit((unsigned char)s[k], radix);
        if (__builtin_mul_overflow(m, (uint64_t)radix, &m) || __builtin_add_overflow(m, (uint64_t)v, &m)) big = 1;
    }
    x->exact = 1; x->big = big; x->n = m; x->d = 1;
    x->s = s; x->radix = radix; x->ns = d0; x->ne = q; x->ds = x->de = 0;
    if (q < n && s[q] == '/') {
        size_t e0 = ++q;
        uint64_t dm = 0;
        while (q < n && r7rs_ns_digit((unsigned char)s[q], radix) >= 0) {
            int v = r7rs_ns_digit((unsigned char)s[q], radix);
            if (__builtin_mul_overflow(dm, (uint64_t)radix, &dm) || __builtin_add_overflow(dm, (uint64_t)v, &dm)) x->big = 1;
            q++;
        }
        if (q == e0) return 0;
        x->d = dm; x->ds = e0; x->de = q;
    }
    *p = q;
    return 1;
}

/* <real R> at s[*p]: an optional sign and a ureal, or an infnan. */
static int r7rs_ns_real_at(const char *s, size_t n, size_t *p, int radix, r7rs_ns_real *x) {
    memset(x, 0, sizeof *x);
    double inf;
    if (r7rs_ns_infnan(s, n, *p, &inf)) { x->f = inf; *p += 6; return 1; }
    size_t q = *p;
    int neg = 0;
    if (q < n && (s[q] == '+' || s[q] == '-')) { neg = (s[q] == '-'); q++; }
    if (!r7rs_ns_ureal(s, n, &q, radix, x)) return 0;
    x->neg = neg;
    if (!x->exact && neg) x->f = -x->f;
    *p = q;
    return 1;
}

/* An exact real's numerator and denominator as bignums (the sign on n). */
static void r7rs_ns_parts(const r7rs_ns_real *x, r7rs_bn *n, r7rs_bn *d) {
    if (x->big) {
        r7bn_from_digits(n, x->s + x->ns, x->ne - x->ns, x->radix, x->neg);
        if (x->de > x->ds) r7bn_from_digits(d, x->s + x->ds, x->de - x->ds, x->radix, 0);
        else r7bn_from_u64(d, 1, 0);
    } else {
        r7bn_from_u64(n, x->n, x->neg);
        r7bn_from_u64(d, x->d, 0);
    }
}
/* The exact number n/d is: an int, a bignum, a ratio (T2), or -- with a zero
 * denominator -- not a number. */
static void r7rs_ns_exact_out(const r7rs_bn *n, const r7rs_bn *d, r7rs_ns_result *out) {
    if (d->n == 0) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_div0; return; }
    r7rs_bn q, m; r7bn_init(&q); r7bn_init(&m);
    r7bn_divmod(&q, &m, n, d);
    if (m.n) { out->kind = R7NS_RATIO; out->big = r7bn_ratio_str(n, d); }
    else if (r7bn_to_i64(&q, &out->i)) out->kind = R7NS_INT;
    else { out->kind = R7NS_BIG; out->big = r7bn_to_str(&q, 10); }
    r7bn_free(&q); r7bn_free(&m);
}
/* `#e` on a decimal lexeme: its exact value from the digits -- mantissa
 * times 10^(exponent - fraction digits) -- not from the double. */
static void r7rs_ns_exact_decimal(const r7rs_ns_real *x, r7rs_ns_result *out) {
    const char *t = x->s + x->ns;
    size_t len = x->ne - x->ns, i = 0, nd = 0;
    char *digits = (char *)malloc(len + 1);
    if (!digits) abort();
    long frac = 0; int seen_dot = 0;
    for (; i < len; i++) {
        char c = t[i];
        if (c >= '0' && c <= '9') { digits[nd++] = c; if (seen_dot) frac++; }
        else if (c == '.') seen_dot = 1;
        else break;                                /* an exponent marker */
    }
    long ex = 0; int eneg = 0, big_ex = 0;
    if (i < len) {
        i++;
        if (i < len && (t[i] == '+' || t[i] == '-')) { eneg = (t[i] == '-'); i++; }
        for (; i < len; i++) { ex = ex * 10 + (t[i] - '0'); if (ex > 1000000) { big_ex = 1; break; } }
    }
    if (big_ex) { free(digits); out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_bigexp; return; }
    long scale = (eneg ? -ex : ex) - frac;
    r7rs_bn n, d, p, ten, t2; r7bn_init(&n); r7bn_init(&d); r7bn_init(&p); r7bn_init(&ten); r7bn_init(&t2);
    r7bn_from_digits(&n, digits, nd, 10, x->neg);
    r7bn_from_u64(&ten, 10, 0);
    r7bn_pow(&p, &ten, (uint64_t)(scale < 0 ? -scale : scale));
    if (scale >= 0) { r7bn_mul(&t2, &n, &p); r7bn_copy(&n, &t2); r7bn_from_u64(&d, 1, 0); }
    else r7bn_copy(&d, &p);
    r7rs_ns_exact_out(&n, &d, out);
    free(digits);
    r7bn_free(&n); r7bn_free(&d); r7bn_free(&p); r7bn_free(&ten); r7bn_free(&t2);
}

/* Apply the exactness prefix (1 `#e`, -1 `#i`, 0 none) to a real and say
 * what it reads as. */
static void r7rs_ns_finish(r7rs_ns_real *x, int exactness, r7rs_ns_result *out) {
    if (x->exact && x->d == 0 && !x->big) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_div0; return; }
    if (x->exact && !x->big && x->d == 1 && exactness >= 0) {
        /* The common case, an integer that fits 64 bits: no allocation. */
        uint64_t nm = x->n;
        if (nm <= (uint64_t)INT64_MAX + (x->neg ? 1u : 0u)) {
            out->kind = R7NS_INT;
            out->i = x->neg ? (int64_t)(~nm + 1u) : (int64_t)nm;
            return;
        }
    }
    if (x->exact) {
        r7rs_bn n, d; r7bn_init(&n); r7bn_init(&d);
        r7rs_ns_parts(x, &n, &d);
        if (exactness < 0) {
            if (d.n == 0) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_div0; }
            else { out->kind = R7NS_FLOAT; out->f = r7bn_ratio_to_double(&n, &d); }
        } else {
            r7rs_ns_exact_out(&n, &d, out);
        }
        r7bn_free(&n); r7bn_free(&d);
        return;
    }
    if (exactness <= 0) { out->kind = R7NS_FLOAT; out->f = x->f; return; }
    /* `#e`: a decimal lexeme by its digits (it is finite even when its
     * double overflowed), an infinity or NaN has no exact value. */
    if (x->dec) r7rs_ns_exact_decimal(x, out);
    else { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_infnan; }
}

/* A malloc'd copy of t. */
static char *r7rs_ns_dup(const char *t) {
    size_t l = strlen(t);
    char *r = (char *)malloc(l + 1);
    if (!r) abort();
    memcpy(r, t, l + 1);
    return r;
}

/* T6: part `which` (0 the real, 1 the imaginary) of an R7NS_COMPLEX
 * spelling "RE IM".  malloc'd.  Inline: the reader does not call it, the
 * runtime copies do. */
static inline char *r7rs_ns_complex_part(const char *s, int which) {
    const char *sp = strchr(s, ' ');
    if (!sp) return r7rs_ns_dup(which ? "0" : s);
    if (which) return r7rs_ns_dup(sp + 1);
    size_t l = (size_t)(sp - s);
    char *r = (char *)malloc(l + 1);
    if (!r) abort();
    memcpy(r, s, l);
    r[l] = 0;
    return r;
}

/* T6: one part of a complex number, spelled as a real this parser reads back
 * in radix 10 -- an exact integer or bignum in decimal, a ratio "n/d", an
 * inexact real `#i`-prefixed so it stays inexact.  malloc'd. */
static char *r7rs_ns_part_spell(const r7rs_ns_result *r) {
    char buf[64];
    if (r->kind == R7NS_INT) { snprintf(buf, sizeof buf, "%lld", (long long)r->i); return r7rs_ns_dup(buf); }
    if (r->kind == R7NS_FLOAT) {
        if (r->f != r->f) return r7rs_ns_dup("+nan.0");
        if (r->f > 1.7976931348623157e308) return r7rs_ns_dup("+inf.0");
        if (r->f < -1.7976931348623157e308) return r7rs_ns_dup("-inf.0");
        snprintf(buf, sizeof buf, "#i%.17g", r->f);
        return r7rs_ns_dup(buf);
    }
    return r7rs_ns_dup(r->big ? r->big : "0");
}
/* T6: a complex number from its two parts (rectangular) or its magnitude and
 * angle (polar, `m@a`, converted to rectangular and inexact). */
static void r7rs_ns_complex(r7rs_ns_real *re, r7rs_ns_real *im, int polar, int exactness,
                            r7rs_ns_result *out) {
    r7rs_ns_result a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    r7rs_ns_finish(re, polar ? -1 : exactness, &a);
    r7rs_ns_finish(im, polar ? -1 : exactness, &b);
    if (a.kind == R7NS_REFUSED || b.kind == R7NS_REFUSED) {
        out->kind = R7NS_REFUSED;
        out->why = a.kind == R7NS_REFUSED ? a.why : b.why;
        free(a.big); free(b.big);
        return;
    }
    if (polar) {
        double m = a.f, t = b.f;
        a.f = m * cos(t);
        b.f = m * sin(t);
    }
    char *ra = r7rs_ns_part_spell(&a), *rb = r7rs_ns_part_spell(&b);
    size_t la = strlen(ra), lb = strlen(rb);
    out->big = (char *)malloc(la + lb + 2);
    if (!out->big) abort();
    memcpy(out->big, ra, la);
    out->big[la] = ' ';
    memcpy(out->big + la + 1, rb, lb + 1);
    out->kind = R7NS_COMPLEX;
    free(ra); free(rb); free(a.big); free(b.big);
}

/* An exact zero: the imaginary part (or polar angle) that leaves a real. */
static int r7rs_ns_exact_zero(const r7rs_ns_real *x) {
    return x->exact && !x->big && x->n == 0 && x->d != 0;
}
/* An exact small integer v, as the parser's own real. */
static void r7rs_ns_exact_small(r7rs_ns_real *x, int v) {
    memset(x, 0, sizeof *x);
    x->exact = 1; x->d = 1; x->radix = 10;
    x->neg = v < 0; x->n = (uint64_t)(v < 0 ? -v : v);
}

/* r7rs_ns_parse -- read s[0..n) as a number in `radix` (a prefix overrides
 * it).  R7NS_NONE when it is not number syntax at all. */
static void r7rs_ns_parse(const char *s, size_t n, int radix, r7rs_ns_result *out) {
    memset(out, 0, sizeof *out);
    size_t p = 0;
    int exactness = 0, radix_given = 0;
    while (p + 1 < n && s[p] == '#') {
        int c = r7rs_ns_lower((unsigned char)s[p + 1]);
        if ((c == 'x' || c == 'o' || c == 'b' || c == 'd') && !radix_given) {
            radix = c == 'x' ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 10;
            radix_given = 1;
        } else if ((c == 'e' || c == 'i') && !exactness) {
            exactness = (c == 'e') ? 1 : -1;
        } else {
            return;
        }
        p += 2;
    }
    if (p >= n) return;
    r7rs_ns_real re, im;
    /* `+i` / `-i`: the imaginary unit. */
    if (p + 2 == n && (s[p] == '+' || s[p] == '-') && r7rs_ns_lower((unsigned char)s[p + 1]) == 'i') {
        r7rs_ns_exact_small(&re, 0);
        r7rs_ns_exact_small(&im, s[p] == '-' ? -1 : 1);
        r7rs_ns_complex(&re, &im, 0, exactness, out);
        return;
    }
    size_t q = p;
    if (!r7rs_ns_real_at(s, n, &q, radix, &re)) return;
    if (q == n) { r7rs_ns_finish(&re, exactness, out); return; }
    int polar = 0;
    if (s[q] == '@') {
        size_t t = q + 1;
        if (!r7rs_ns_real_at(s, n, &t, radix, &im) || t != n) return;
        polar = 1;
    } else if (s[q] == '+' || s[q] == '-') {
        if (q + 2 == n && r7rs_ns_lower((unsigned char)s[q + 1]) == 'i') {
            r7rs_ns_exact_small(&im, s[q] == '-' ? -1 : 1);
        } else {
            size_t t = q;
            if (!r7rs_ns_real_at(s, n, &t, radix, &im) || t + 1 != n ||
                r7rs_ns_lower((unsigned char)s[t]) != 'i') return;
        }
    } else if (q + 1 == n && r7rs_ns_lower((unsigned char)s[q]) == 'i' && (s[p] == '+' || s[p] == '-')) {
        /* `+2i`, `-inf.0i`: a pure imaginary number. */
        im = re;
        r7rs_ns_exact_small(&re, 0);
    } else {
        return;
    }
    /* An exact zero imaginary part (or polar angle) is a real, unless `#i`
     * makes it the inexact 0.0 of a complex number. */
    if (r7rs_ns_exact_zero(&im) && exactness >= 0) { r7rs_ns_finish(&re, exactness, out); return; }
    r7rs_ns_complex(&re, &im, polar, exactness, out);
}
