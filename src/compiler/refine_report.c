/* refine_report.c -- SX8a: the JSON obligation dump.  See refine_report.h. */

#include "refine_report.h"

#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "forms.h"
#include "refine_smtlib.h"

/* JSON string escaping.  Deliberately conservative: control bytes go out as
 * \uXXXX rather than being passed through, because a predicate's source text
 * is arbitrary user input and a dump that only parses for well-behaved inputs
 * is not a machine-readable surface. */
static void json_str(Buf *out, const char *s) {
    buf_puts(out, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"':  buf_puts(out, "\\\""); break;
            case '\\': buf_puts(out, "\\\\"); break;
            case '\n': buf_puts(out, "\\n");  break;
            case '\r': buf_puts(out, "\\r");  break;
            case '\t': buf_puts(out, "\\t");  break;
            default:
                if (*p < 0x20) {
                    char esc[8];
                    int n = snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    buf_write(out, esc, (size_t)n);
                } else {
                    buf_putc(out, (char)*p);
                }
        }
    }
    buf_puts(out, "\"");
}

static void json_kv_str(Buf *out, const char *k, const char *v) {
    buf_printf(out, "\"%s\": ", k);
    json_str(out, v);
}

static void json_kv_u32(Buf *out, const char *k, uint32_t v) {
    buf_printf(out, "\"%s\": %u", k, v);
}

/* The verdict as three mutually exclusive words rather than two booleans the
 * consumer has to combine.  "unknown" is a real answer here, not a missing
 * one -- an obligation that fell through the chain kept its runtime check. */
static const char *verdict_of(const RefineObligation *ob) {
    if (!ob->discharged)  return "undecided";
    if (ob->proven)       return "proven";
    if (ob->counterex)    return "refuted";
    return "unknown";
}

/* Only the caps that actually bit.  Emitting nine zeroes per record for the
 * overwhelmingly common "nothing was capped" obligation would bury the ones
 * that matter under noise. */
static void emit_caps(Buf *out, const char *key, const RefineCapStats *c) {
    struct { const char *name; uint32_t hits; } rows[] = {
        { "cubes",         c->cubes_hits        },
        { "cube_literals", c->cube_lits_hits    },
        { "expand_depth",  c->expand_depth_hits },
        { "la_vars",       c->la_vars_hits      },
        { "la_constraints",c->la_constr_hits    },
        { "la_fm_blowup",  c->la_fm_hits        },
        { "euf_terms",     c->euf_terms_hits    },
        { "no_shared",     c->no_shared_hits    },
        { "no_rounds",     c->no_rounds_hits    },
    };
    buf_printf(out, "\"%s\": {", key);
    bool first = true;
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        if (!rows[i].hits) continue;
        if (!first) buf_puts(out, ", ");
        first = false;
        json_kv_u32(out, rows[i].name, rows[i].hits);
    }
    buf_puts(out, "}");
}

static void emit_model(Buf *out, const RefineModel *m) {
    buf_puts(out, "[");
    for (uint32_t i = 0; m && i < m->n; i++) {
        const RefineModelBinding *b = &m->bindings[i];
        if (i) buf_puts(out, ", ");
        buf_puts(out, "{");
        json_kv_str(out, "name", b->name);
        buf_puts(out, ", ");
        if (b->is_real) buf_printf(out, "\"value\": %g, \"sort\": \"Real\"", b->rval);
        else            buf_printf(out, "\"value\": %lld, \"sort\": \"Int\"",
                                    (long long)b->ival);
        buf_puts(out, "}");
    }
    buf_puts(out, "]");
}

static void emit_one(Buf *out, const RefineObligation *ob) {
    buf_puts(out, "    {");
    json_kv_str(out, "what", ob->what);
    buf_puts(out, ", ");
    json_kv_str(out, "function", ob->fn_name);
    buf_puts(out, ", ");
    json_kv_str(out, "variable", ob->var_name);
    buf_puts(out, ", ");
    json_kv_str(out, "base_type", ob->base_type_name);
    buf_puts(out, ",\n     ");

    /* Location.  The file comes from the diag registry rather than being
     * carried on the span, which holds only the id. */
    buf_puts(out, "\"location\": {");
    json_kv_str(out, "file", diag_file_path(ob->loc.file_id));
    buf_puts(out, ", ");
    json_kv_u32(out, "line", ob->loc.line);
    buf_puts(out, ", ");
    json_kv_u32(out, "col", ob->loc.col_start);
    buf_puts(out, "},\n     ");

    /* The predicate as the user wrote it.  The SMT-LIB below is precise but
     * normalized past recognition; this is the half a human matches against
     * their own source. */
    buf_puts(out, "\"predicate\": ");
    if (ob->predicate) {
        Buf pb; buf_init(&pb);
        form_print(&pb, ob->predicate);
        buf_putc(&pb, '\0');   /* Buf is not NUL-terminated */
        json_str(out, pb.data ? pb.data : "");
        buf_free(&pb);
    } else {
        buf_puts(out, "null");
    }
    buf_puts(out, ",\n     ");

    json_kv_str(out, "verdict", verdict_of(ob));
    buf_puts(out, ", ");
    json_kv_str(out, "decided_by", ob->decided_by ? ob->decided_by : "");
    buf_printf(out, ", \"memo_hit\": %s", ob->memo_hit ? "true" : "false");
    buf_printf(out, ", \"runtime_guarded\": %s",
                ob->runtime_guarded ? "true" : "false");
    buf_puts(out, ",\n     ");

    buf_puts(out, "\"counterexample\": ");
    emit_model(out, ob->counterex);
    buf_puts(out, ",\n     ");

    emit_caps(out, "caps_hit", &ob->caps);
    buf_puts(out, ", ");
    /* Caps hit by the RT4 path-splitting probes run for this site before this
     * obligation existed.  Separate from caps_hit because a probe asks a
     * different question (one path, not the whole body) -- but reported,
     * because the alternative is what this used to do: count them globally,
     * so the per-compile summary said a cap bit while every obligation read
     * zero.  Sum the two for "all solver work this site paid for". */
    emit_caps(out, "caps_hit_probe", &ob->caps_probe);
    buf_puts(out, ",\n     ");

    /* The VC, as SMT-LIB2, in the refutation form the stages actually decide:
     * hypotheses asserted, goal asserted NEGATED, so `unsat` means valid.
     * This is what makes a record replayable -- paste it into `tur smt`, or
     * into any external solver, and ask the same question by hand. */
    buf_puts(out, "\"vc_smtlib\": ");
    if (ob->vc) {
        Buf vb; buf_init(&vb);
        refine_smtlib_emit(ob->vc, &vb);
        buf_putc(&vb, '\0');   /* Buf is not NUL-terminated */
        json_str(out, vb.data ? vb.data : "");
        buf_free(&vb);
    } else {
        buf_puts(out, "null");
    }
    buf_puts(out, "}");
}

void refine_report_json(const RefineObligationVec *v, Buf *out) {
    /* SX9: schema 1 is the STABLE shape of this record -- the keys below,
     * with `vc_smtlib` in the refutation form the stages decide.  Schema 0
     * was the same shape flagged unstable through SX8a-b; a consumer that
     * branches on the number sees 1 from 0.45.0 on.  Bump it again only for
     * a change that removes or retypes a key; adding one is compatible. */
    buf_puts(out, "{\n  \"schema\": 1,\n");
    buf_printf(out, "  \"schema_note\": \"stable since SX9 (0.45.0); "
                     "additive changes keep 1 -- branch on schema\",\n");
    buf_printf(out, "  \"obligations\": [\n");
    uint32_t n = v ? v->n : 0;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < n; i++) {
        const RefineObligation *ob = v->obs[i];
        /* Speculative probes are not obligations -- they are the inference
         * pass asking a question it will discard.  Reporting them would make
         * the array disagree with the compile's own counts. */
        if (!ob || ob->speculative || ob->path_probe) continue;
        if (emitted++) buf_puts(out, ",\n");
        emit_one(out, ob);
    }
    buf_puts(out, "\n  ]\n}\n");
}
