/* r7rs-lang-plan T0 -- the R7RS number syntax (7.1.1 <number>): ONE parser for
 * every place a `#lang r7rs` number is read.  The same text is
 *   src/compiler/r7rs_numsyntax.inc  #included by the source reader (reader.c)
 *                                    and the interpreter's natives
 *   stdlib/r7rs/numsyntax.tur        the file-scope C block `string->number`
 *                                    and `read` call on the compiled back end
 * and tests/check-r7rs-numsyntax-sync.sh checks the copies agree.  Edit this
 * file, then run tools/gen-r7rs-numsyntax.py to copy it into the .tur block.
 *
 * It reads the whole grammar -- prefixes, ratios, decimals with the R5RS
 * exponent markers, infinities, rectangular and polar complex numbers -- so
 * a token is never split into a number and a stray symbol.  What the tower
 * cannot hold yet is REFUSED with the reason, never read as something else:
 * an exact non-integer (T2), a non-real complex number (T6), an exact
 * integer outside int64 (T1).  A ratio or complex number whose value the
 * tower does hold reads as that value: `10/2` is 5, `#i1/2` is 0.5 and
 * `3+0i` is 3 (an exact zero imaginary part makes a real, R7RS 6.2.6). */

enum { R7NS_NONE = 0, R7NS_INT = 1, R7NS_FLOAT = 2, R7NS_REFUSED = 3 };

typedef struct {
    int kind;          /* R7NS_*: not a number, an exact int, a double, refused */
    int64_t i;         /* R7NS_INT */
    double f;          /* R7NS_FLOAT */
    const char *why;   /* R7NS_REFUSED: why, as a sentence fragment */
} r7rs_ns_result;

static const char r7rs_ns_why_ratio[] =
    "an exact non-integer needs exact rationals, which `#lang r7rs` does not "
    "have yet (r7rs-lang-plan T2); write it inexact (`#i` prefix or a decimal "
    "point)";
static const char r7rs_ns_why_complex[] =
    "a non-real complex number needs complex numbers, which `#lang r7rs` does "
    "not have yet (r7rs-lang-plan T6)";
static const char r7rs_ns_why_big[] =
    "an exact integer outside int64 needs bignums, which `#lang r7rs` does not "
    "have yet (r7rs-lang-plan T1)";
static const char r7rs_ns_why_div0[] =
    "an exact rational with a zero denominator is not a number";
static const char r7rs_ns_why_infnan[] =
    "an infinity or NaN has no exact representation";

/* One real as read.  Exact: sign and the magnitudes n/d (`big` when one
 * left 64 bits).  Inexact: the double.  `nf`/`df` shadow n and d as doubles
 * for `#i` on a ratio too long for 64 bits. */
typedef struct {
    int exact, neg, big;
    uint64_t n, d;
    double nf, df, f;
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
            if (buf != small) free(buf);
            *p = q;
            return 1;
        }
    }
    if (nd == 0) return 0;
    uint64_t m = 0; double mf = 0.0; int big = 0;
    for (size_t k = d0; k < q; k++) {
        int v = r7rs_ns_digit((unsigned char)s[k], radix);
        if (__builtin_mul_overflow(m, (uint64_t)radix, &m) || __builtin_add_overflow(m, (uint64_t)v, &m)) big = 1;
        mf = mf * radix + v;
    }
    x->exact = 1; x->big = big; x->n = m; x->nf = mf; x->d = 1; x->df = 1.0;
    if (q < n && s[q] == '/') {
        size_t e0 = ++q;
        uint64_t dm = 0; double df = 0.0;
        while (q < n && r7rs_ns_digit((unsigned char)s[q], radix) >= 0) {
            int v = r7rs_ns_digit((unsigned char)s[q], radix);
            if (__builtin_mul_overflow(dm, (uint64_t)radix, &dm) || __builtin_add_overflow(dm, (uint64_t)v, &dm)) x->big = 1;
            df = df * radix + v;
            q++;
        }
        if (q == e0) return 0;
        x->d = dm; x->df = df;
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

static uint64_t r7rs_ns_gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

/* Apply the exactness prefix (1 `#e`, -1 `#i`, 0 none) to a real and say
 * what it reads as. */
static void r7rs_ns_finish(r7rs_ns_real *x, int exactness, r7rs_ns_result *out) {
    if (x->exact && x->d == 0 && !x->big) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_div0; return; }
    if (x->exact && exactness < 0) {
        x->exact = 0;
        x->f = (x->big ? x->nf / x->df : (double)x->n / (double)x->d);
        if (x->neg) x->f = -x->f;
    }
    if (!x->exact) {
        if (exactness <= 0) { out->kind = R7NS_FLOAT; out->f = x->f; return; }
        double f = x->f;
        if (!__builtin_isfinite(f)) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_infnan; return; }
        if (__builtin_floor(f) != f) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_ratio; return; }
        if (f >= 9223372036854775808.0 || f < -9223372036854775808.0) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_big; return; }
        out->kind = R7NS_INT; out->i = (int64_t)f;
        return;
    }
    if (x->big) {
        /* Too long for 64 bits: an integer here needs bignums, a ratio at
         * least rationals.  `nf / df` is only a guess at which. */
        double q = x->nf / x->df;
        out->kind = R7NS_REFUSED;
        out->why = (x->df == 1.0 || __builtin_floor(q) == q) ? r7rs_ns_why_big : r7rs_ns_why_ratio;
        return;
    }
    uint64_t g = r7rs_ns_gcd(x->n, x->d);
    uint64_t nm = g ? x->n / g : 0, dm = g ? x->d / g : 1;
    if (dm != 1) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_ratio; return; }
    if (nm > (uint64_t)INT64_MAX + (x->neg ? 1u : 0u)) { out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_big; return; }
    out->kind = R7NS_INT;
    out->i = x->neg ? (int64_t)(~nm + 1u) : (int64_t)nm;
}

/* An exact zero: the imaginary part (or polar angle) that leaves a real. */
static int r7rs_ns_exact_zero(const r7rs_ns_real *x) {
    return x->exact && !x->big && x->n == 0 && x->d != 0;
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
        out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_complex;
        return;
    }
    size_t q = p;
    if (!r7rs_ns_real_at(s, n, &q, radix, &re)) return;
    if (q == n) { r7rs_ns_finish(&re, exactness, out); return; }
    int complex_form = 0, imag_zero = 0;
    if (s[q] == '@') {
        size_t t = q + 1;
        if (!r7rs_ns_real_at(s, n, &t, radix, &im) || t != n) return;
        complex_form = 1; imag_zero = r7rs_ns_exact_zero(&im);
    } else if (s[q] == '+' || s[q] == '-') {
        if (q + 2 == n && r7rs_ns_lower((unsigned char)s[q + 1]) == 'i') {
            complex_form = 1; imag_zero = 0;
        } else {
            size_t t = q;
            if (!r7rs_ns_real_at(s, n, &t, radix, &im) || t + 1 != n ||
                r7rs_ns_lower((unsigned char)s[t]) != 'i') return;
            complex_form = 1; imag_zero = r7rs_ns_exact_zero(&im);
        }
    } else if (q + 1 == n && r7rs_ns_lower((unsigned char)s[q]) == 'i' && (s[p] == '+' || s[p] == '-')) {
        /* `+2i`, `-inf.0i`: a pure imaginary number. */
        complex_form = 1; imag_zero = r7rs_ns_exact_zero(&re);
        if (imag_zero) { memset(&re, 0, sizeof re); re.exact = 1; re.d = 1; }
    }
    if (!complex_form) return;
    /* An exact zero imaginary part (or polar angle) is a real, unless `#i`
     * makes it the inexact 0.0 of a complex number. */
    if (imag_zero && exactness >= 0) { r7rs_ns_finish(&re, exactness, out); return; }
    out->kind = R7NS_REFUSED; out->why = r7rs_ns_why_complex;
}
