/* test_typerow.c -- unit tests for the TY_TYPEROW type variant (Layer 2 of the
 * variadic-HKT-rows work, docs/archive/history/variadic-hkt-rows-missing.md).
 *
 * A TY_TYPEROW is a compile-time-only *row of types* -- an ordered list of
 * element types, surface-spelled `#row{T1 T2 ...}`. These tests build rows
 * programmatically (no parser/codegen dependency) and pin:
 *   - construction (kind, hkt_kind, length, element identity)
 *   - order-SENSITIVE structural equality via type_eq
 *   - order-INSENSITIVE (multiset) equality via type_typerow_eq_perm
 *   - list semantics: duplicates preserved, nested rows not flattened
 *   - the empty (unit) row
 *   - printed surface form `#row{...}`
 */

#include <stdio.h>
#include <string.h>

#include "types.h"
#include "arena.h"
#include "buf.h"
#include "lsp_sym.h"

static int failures = 0;

/* Stub required by the tur_core link dependency (mirrors the other
 * compiler-internals unit tests). */
int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out) {
    (void)source_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static void check(const char *label, int cond) {
    if (cond) { printf("PASS [%s]\n", label); return; }
    fprintf(stderr, "FAIL [%s]\n", label);
    failures++;
}

/* Render a type via type_print into an owned Buf (LSan-clean, unlike the
 * composite-name path of type_name) and compare to `want`. */
static void check_print(const char *label, Type t, const char *want) {
    Buf b; buf_init(&b);
    type_print(&b, t);
    buf_putc(&b, '\0');
    if (b.data && strcmp(b.data, want) == 0) {
        printf("PASS [%s]\n", label);
    } else {
        fprintf(stderr, "FAIL [%s]: want \"%s\", got \"%s\"\n",
                label, want, b.data ? b.data : "(null)");
        failures++;
    }
    buf_free(&b);
}

int main(void) {
    Arena a;
    arena_init(&a, 1 << 16);

    /* Element types (live for the whole test; rows store these pointers). */
    Type t_int  = type_simple(TY_INT,  CK_COPY);
    Type t_bool = type_simple(TY_BOOL, CK_COPY);

    Type *ib[2] = { &t_int, &t_bool };  /* [int bool] */
    Type *bi[2] = { &t_bool, &t_int };  /* [bool int] */
    Type *ii[2] = { &t_int, &t_int };   /* [int int]  */
    Type *i1[1] = { &t_int };           /* [int]      */

    Type row_ib  = type_typerow(&a, ib, 2);
    Type row_ib2 = type_typerow(&a, ib, 2);  /* independent copy, same elems */
    Type row_bi  = type_typerow(&a, bi, 2);
    Type row_ii  = type_typerow(&a, ii, 2);
    Type row_i1  = type_typerow(&a, i1, 1);
    Type row_empty  = type_typerow(&a, NULL, 0);
    Type row_empty2 = type_typerow(&a, NULL, 0);

    /* --- construction --- */
    check("kind == TY_TYPEROW",        row_ib.kind == TY_TYPEROW);
    check("hkt_kind == KIND_TYPEROW",  row_ib.hkt_kind == KIND_TYPEROW);
    check("n_elements == 2",           row_ib.as.typerow_.n_elements == 2);
    check("element[0] is int",         row_ib.as.typerow_.elements[0]->kind == TY_INT);
    check("element[1] is bool",        row_ib.as.typerow_.elements[1]->kind == TY_BOOL);
    check("empty row n_elements == 0", row_empty.as.typerow_.n_elements == 0);

    /* --- order-SENSITIVE structural equality (type_eq) --- */
    check("type_eq(#row{int bool}, #row{int bool})",  type_eq(row_ib, row_ib2));
    check("#row{int bool} != #row{bool int} (order)", !type_eq(row_ib, row_bi));
    check("#row{int bool} != #row{int int}",          !type_eq(row_ib, row_ii));
    check("#row{int bool} != #row{int} (length)",     !type_eq(row_ib, row_i1));
    check("type_eq(empty, empty)",                    type_eq(row_empty, row_empty2));
    check("#row{int} != empty",                       !type_eq(row_i1, row_empty));
    /* A row is never equal to a non-row of a different TypeKind. */
    check("#row{int} != bare int",                    !type_eq(row_i1, t_int));

    /* --- order-INSENSITIVE (multiset) equality (type_typerow_eq_perm) --- */
    check("perm(#row{int bool}, #row{bool int})",  type_typerow_eq_perm(row_ib, row_bi));
    check("perm is reflexive on #row{int bool}",   type_typerow_eq_perm(row_ib, row_ib2));
    check("perm(empty, empty)",                    type_typerow_eq_perm(row_empty, row_empty2));
    /* Multiset, not set: {int,bool} vs {int,int} differ even though lengths match. */
    check("!perm(#row{int bool}, #row{int int})",  !type_typerow_eq_perm(row_ib, row_ii));
    /* Multiplicity matters: {int,int} vs {int} (length guard). */
    check("!perm(#row{int int}, #row{int})",       !type_typerow_eq_perm(row_ii, row_i1));
    /* perm rejects non-rows. */
    check("!perm(#row{int}, bare int)",            !type_typerow_eq_perm(row_i1, t_int));

    /* --- list semantics: duplicates preserved, nesting not flattened --- */
    check("duplicates preserved: #row{int int} len 2", row_ii.as.typerow_.n_elements == 2);
    Type *nest[2] = { &row_i1, &t_bool };           /* #row{ #row{int} bool } */
    Type row_nested = type_typerow(&a, nest, 2);
    check("nested row NOT flattened (len 2)", row_nested.as.typerow_.n_elements == 2);
    check("nested element[0] is a row",       row_nested.as.typerow_.elements[0]->kind == TY_TYPEROW);

    /* --- printed surface form (via type_print -> owned Buf, LSan-clean) --- */
    check_print("print(#row{int bool}) == \"#row{int bool}\"",
                row_ib, "#row{int bool}");
    check_print("print(empty row) == \"#row{}\"",
                row_empty, "#row{}");
    check_print("print(nested) == \"#row{#row{int} bool}\"",
                row_nested, "#row{#row{int} bool}");

    /* --- Layer 5: row algebra (contains / concat / union / intersect) --- */
    Type t_cstr = type_simple(TY_CSTR, CK_COPY);
    Type *bc[2] = { &t_bool, &t_cstr };  /* #row{bool cstr} -- overlaps row_ib on bool */
    Type row_bc = type_typerow(&a, bc, 2);

    /* membership */
    check("contains(#row{int bool}, int)",   type_typerow_contains(row_ib, t_int));
    check("contains(#row{int bool}, bool)",  type_typerow_contains(row_ib, t_bool));
    check("!contains(#row{int bool}, cstr)", !type_typerow_contains(row_ib, t_cstr));
    check("!contains(empty, int)",           !type_typerow_contains(row_empty, t_int));
    check("!contains(non-row, int)",         !type_typerow_contains(t_int, t_int));

    /* concat (++): order-preserving, duplicates kept */
    check_print("#row{int bool} ++ #row{bool cstr} == #row{int bool bool cstr}",
                type_typerow_concat(&a, row_ib, row_bc), "#row{int bool bool cstr}");
    check_print("empty ++ #row{int bool} == #row{int bool}",
                type_typerow_concat(&a, row_empty, row_ib), "#row{int bool}");

    /* union (join): deduplicated, order-preserving (bool appears once) */
    check_print("#row{int bool} U #row{bool cstr} == #row{int bool cstr}",
                type_typerow_union(&a, row_ib, row_bc), "#row{int bool cstr}");
    check_print("union is order-preserving from x then y",
                type_typerow_union(&a, row_bc, row_ib), "#row{bool cstr int}");
    check_print("union dedups within a self-union",
                type_typerow_union(&a, row_ib, row_ib), "#row{int bool}");

    /* intersection: common elements in x's order, deduplicated */
    check_print("#row{int bool} & #row{bool cstr} == #row{bool}",
                type_typerow_intersect(&a, row_ib, row_bc), "#row{bool}");
    check_print("disjoint intersection is empty",
                type_typerow_intersect(&a, row_i1, row_bc), "#row{}");

    arena_free(&a);

    if (failures == 0) {
        printf("\nAll TY_TYPEROW tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d TY_TYPEROW test(s) FAILED.\n", failures);
    return 1;
}
