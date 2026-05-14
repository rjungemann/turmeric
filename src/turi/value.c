#include "value.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TuriClosure is an incomplete type here; value.c only uses it as a pointer. */

TuriValue turi_error(const char *msg) {
    return (TuriValue){TURI_ERROR, .as_error = strdup(msg)};
}

TuriValue turi_errorf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return (TuriValue){TURI_ERROR, .as_error = strdup(buf)};
}

void turi_print_value(FILE *out, TuriValue v) {
    switch (v.tag) {
    case TURI_NIL:
        fprintf(out, "nil");
        break;
    case TURI_BOOL:
        fprintf(out, "%s", v.as_bool ? "true" : "false");
        break;
    case TURI_INT:
        fprintf(out, "%lld", (long long)v.as_int);
        break;
    case TURI_FLOAT:
        fprintf(out, "%g", v.as_float);
        break;
    case TURI_CSTR:
        fprintf(out, "\"%s\"", v.as_cstr ? v.as_cstr : "");
        break;
    case TURI_CLOSURE: {
        /* Print as #<fn name> if we can access the function name */
        fprintf(out, "#<fn>");
        break;
    }
    case TURI_ERROR:
        fprintf(out, "#<error: %s>", v.as_error ? v.as_error : "");
        break;
    case TURI_EFFECT_CONT:
        fprintf(out, "#<continuation>");
        break;
    case TURI_STRUCT:
        fprintf(out, "#<struct>");
        break;
    case TURI_THROW:
        fprintf(out, "#<exception>");
        break;
    case TURI_FUTURE:
        fprintf(out, "#<future>");
        break;
    }
}
