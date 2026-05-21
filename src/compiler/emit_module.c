/* emit_module.c -- program/module assembly (emit_program, emit_header, emit_implementation). */
#include "emit_internal.h"

/* ------------ program-level emit ------------ */

int emit_program(Buf *out, const Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit: expected EX_PROGRAM\n");
        return -1;
    }

    /* Two buffers: file scope (statics) and main body. We assemble at the end. */
    Buf file; buf_init(&file);
    Buf body; buf_init(&body);
    /* Phase 19: separate buffers for ordered final assembly:
     *   early_file   - pass 0: struct typedefs + drop glue
     *   fwd_decls    - pass 1: function forward declarations
     *   extern_decls - user extern-c declarations
     *   file         - pass 2: function definitions + globals
     *   pending_handler_fns emitted between fwd_decls and file
     * Order ensures: struct typedefs visible to fwd_decls; fwd_decls visible
     * to handler functions; handler functions visible to fn definitions. */
    Buf early_file;  buf_init(&early_file);
    Buf fwd_decls;   buf_init(&fwd_decls);
    Buf extern_decls; buf_init(&extern_decls);
    Buf defer_thunks; buf_init(&defer_thunks);

    EmitCtx ctx;
    ctx.file = &file;
    ctx.main_ = &body;
    ctx.indent = 4;
    ctx.tmp_n = 0;
    ctx.fn_params = NULL;
    ctx.n_fn_params = 0;
    /* Phase 3: closure tracking */
    ctx.closure = NULL;
    ctx.env_var_name = NULL;
    /* Phase 3: env struct tracking */
    ctx.env_struct_names = NULL;
    ctx.n_env_struct_names = 0;
    ctx.cap_env_struct_names = 0;
    /* Phase 4 v1: frame tracking */
    ctx.frame_var = NULL;
    ctx.in_scope_with_defers = false;
    ctx.pending_defer_thunks = NULL;
    /* Phase 4 v1: defer captures tracking */
    ctx.defer_captures = NULL;
    ctx.n_defer_captures = 0;
    /* Phase 3/4: Track return emission */
    ctx.return_emitted = false;
    /* Phase 19: Pending effect handler function buffer */
    Buf pending_hfns; buf_init(&pending_hfns);
    ctx.pending_handler_fns = &pending_hfns;
    /* Phase R5: no-unwind context (false at top level; set per-function) */
    ctx.no_unwind = false;
    /* Phase M3: emit_program always uses single-file (non-separate) mode */
    ctx.separate_compilation = false;
    /* Phase 19D: handle captures (NULL at top level) */
    ctx.handle_captures = NULL;
    ctx.n_handle_captures = 0;
    ctx.handle_env_name = NULL;

    /* Phase M0: Flatten program items, expanding EX_DEFMODULE body. */
    uint32_t n_items;
    const Expr **items = flatten_program_items(program, &n_items);

    /* Check if user defined a main function */
    bool user_has_main = false;
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            if (strcmp(fd->binding->name->name, "main") == 0) {
                user_has_main = true;
                break;
            }
        }
    }

    /* Phase 2: Two-pass emission for mutual recursion support.
     * Pass 0: Emit struct typedefs + drop glue (must precede function forward decls). */
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind == EX_DEF && e->as.def_.struct_def) {
            StructDef *def = e->as.def_.struct_def;
            /* Emit: typedef struct Name { fields... } Name; */
            buf_printf(&early_file, "typedef struct %s {\n", def->name);
            for (uint32_t j = 0; j < def->n_fields; j++) {
                StructField *f = &def->fields[j];
                const char *ctype;
                switch (f->kind) {
                    case TY_INT:      ctype = "int64_t"; break;
                    case TY_BOOL:     ctype = "bool"; break;
                    case TY_FLOAT:    ctype = "double"; break;
                    case TY_CSTR:     ctype = "const char *"; break;
                    case TY_PTR_VOID: ctype = "void *"; break;
                    case TY_RC:
                    case TY_WEAK:     ctype = "RcControlBlock *"; break;
                    case TY_REF:
                    case TY_LREF:     ctype = "void *"; break;
                    /* Phase N6: new numeric field types */
                    case TY_INT8:     ctype = "int8_t"; break;
                    case TY_INT16:    ctype = "int16_t"; break;
                    case TY_INT32:    ctype = "int32_t"; break;
                    case TY_UINT8:    ctype = "uint8_t"; break;
                    case TY_UINT16:   ctype = "uint16_t"; break;
                    case TY_UINT32:   ctype = "uint32_t"; break;
                    case TY_UINT64:   ctype = "uint64_t"; break;
                    case TY_FLOAT32:  ctype = "float"; break;
                    default:          ctype = "int64_t"; break;
                }
                char *mfn = mangle_field_name(f->name);
                buf_printf(&early_file, "    %s %s;\n", ctype, mfn);
                free(mfn);
            }
            buf_printf(&early_file, "} %s;\n\n", def->name);
            /* If any RC/ref/weak field, emit drop glue function */
            if (def->needs_drop_glue) {
                buf_printf(&early_file, "static void drop_glue_%s(void *ptr) {\n", def->name);
                buf_printf(&early_file, "    if (!ptr) return;\n");
                buf_printf(&early_file, "    %s *s = (%s *)ptr;\n", def->name, def->name);
                /* Drop fields in REVERSE order */
                for (int32_t j = (int32_t)def->n_fields - 1; j >= 0; j--) {
                    StructField *f = &def->fields[j];
                    char *mfn = mangle_field_name(f->name);
                    if (f->kind == TY_RC) {
                        buf_printf(&early_file, "    if (s->%s) { rc_strong_decrement(s->%s); rc_free_queue_drain(); }\n",
                                   mfn, mfn);
                    } else if (f->kind == TY_WEAK) {
                        buf_printf(&early_file, "    if (s->%s) rc_weak_decrement(s->%s);\n",
                                   mfn, mfn);
                    } else if (f->kind == TY_REF || f->kind == TY_LREF) {
                        buf_printf(&early_file, "    if (s->%s) free(s->%s);\n",
                                   mfn, mfn);
                    }
                    free(mfn);
                }
                buf_printf(&early_file, "    free(ptr);\n");
                buf_printf(&early_file, "}\n\n");
            }
        }
        /* Phase G0/G1: ADT typedef + constructor functions */
        else if (e->kind == EX_DEFDATA || e->kind == EX_DEFGADT) {
            AdtDef *def = (e->kind == EX_DEFGADT) ? e->as.defgadt_.def : e->as.defdata_.def;
            char adt_c_name[256];
            snprintf(adt_c_name, sizeof(adt_c_name), "tur_adt_%s", def->name);
            buf_printf(&early_file, "typedef struct %s {\n", adt_c_name);
            buf_printf(&early_file, "    int tag;\n");
            buf_printf(&early_file, "    union {\n");
            for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
                CtorDef *ctor = def->ctors[ci];
                buf_printf(&early_file, "        struct {");
                for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                    const char *ctype;
                    switch (ctor->fields[fi].kind) {
                        case TY_INT:      ctype = "int64_t"; break;
                        case TY_BOOL:     ctype = "bool"; break;
                        case TY_FLOAT:    ctype = "double"; break;
                        case TY_CSTR:     ctype = "const char *"; break;
                        case TY_PTR_VOID: ctype = "void *"; break;
                        case TY_RC:
                        case TY_WEAK:     ctype = "RcControlBlock *"; break;
                        case TY_REF:
                        case TY_LREF:     ctype = "void *"; break;
                        case TY_INT8:     ctype = "int8_t"; break;
                        case TY_INT16:    ctype = "int16_t"; break;
                        case TY_INT32:    ctype = "int32_t"; break;
                        case TY_INT64:    ctype = "int64_t"; break;
                        case TY_UINT8:    ctype = "uint8_t"; break;
                        case TY_UINT16:   ctype = "uint16_t"; break;
                        case TY_UINT32:   ctype = "uint32_t"; break;
                        case TY_UINT64:   ctype = "uint64_t"; break;
                        case TY_FLOAT32:  ctype = "float"; break;
                        case TY_FLOAT64:  ctype = "double"; break;
                        default:          ctype = "int64_t"; break;
                    }
                    buf_printf(&early_file, " %s _%u;", ctype, fi);
                }
                buf_printf(&early_file, " } %s;\n", ctor->name);
            }
            buf_printf(&early_file, "    } as;\n");
            buf_printf(&early_file, "} %s;\n\n", adt_c_name);

            /* Emit constructor functions */
            for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
                CtorDef *ctor = def->ctors[ci];
                buf_printf(&early_file, "static int64_t ctor_%s(", ctor->name);
                for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                    if (fi > 0) buf_puts(&early_file, ", ");
                    const char *ctype;
                    switch (ctor->fields[fi].kind) {
                        case TY_INT:      ctype = "int64_t"; break;
                        case TY_BOOL:     ctype = "bool"; break;
                        case TY_FLOAT:    ctype = "double"; break;
                        case TY_CSTR:     ctype = "const char *"; break;
                        case TY_PTR_VOID: ctype = "void *"; break;
                        case TY_RC:
                        case TY_WEAK:     ctype = "RcControlBlock *"; break;
                        case TY_REF:
                        case TY_LREF:     ctype = "void *"; break;
                        case TY_INT8:     ctype = "int8_t"; break;
                        case TY_INT16:    ctype = "int16_t"; break;
                        case TY_INT32:    ctype = "int32_t"; break;
                        case TY_INT64:    ctype = "int64_t"; break;
                        case TY_UINT8:    ctype = "uint8_t"; break;
                        case TY_UINT16:   ctype = "uint16_t"; break;
                        case TY_UINT32:   ctype = "uint32_t"; break;
                        case TY_UINT64:   ctype = "uint64_t"; break;
                        case TY_FLOAT32:  ctype = "float"; break;
                        case TY_FLOAT64:  ctype = "double"; break;
                        default:          ctype = "int64_t"; break;
                    }
                    buf_printf(&early_file, "%s _%u", ctype, fi);
                }
                buf_printf(&early_file, ") {\n");
                buf_printf(&early_file, "    %s *__r = (%s *)malloc(sizeof(%s));\n",
                           adt_c_name, adt_c_name, adt_c_name);
                buf_printf(&early_file, "    __r->tag = %u;\n", ctor->tag);
                for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                    buf_printf(&early_file, "    __r->as.%s._%u = _%u;\n",
                               ctor->name, fi, fi);
                }
                buf_printf(&early_file, "    return (int64_t)(intptr_t)__r;\n");
                buf_printf(&early_file, "}\n\n");
            }
        }
    }

    /* DV2+DV3: Emit TurDynFrame typedef, pthread_key_t globals, constructors,
     * cleanup functions, and (DV3) snapshot/convey infrastructure for every
     * defdynamic declaration found in the program. */
    {
        bool any_dynvar = false;
        for (uint32_t i = 0; i < n_items; i++) {
            if (items[i]->kind == EX_DEFDYNAMIC) { any_dynvar = true; break; }
        }
        if (any_dynvar) {
            /* DV2: TurDynFrame -- heap flag distinguishes DV3 snapshot frames
             * (heap=1, owned) from DV2 stack frames (heap=0, not owned). */
            buf_puts(&early_file,
                "/* DV2+DV3: dynamic var frame stack */\n"
                "typedef struct TurDynFrame {\n"
                "    struct TurDynFrame *prev;\n"
                "    void               *value;\n"
                "    int                 heap; /* DV3: 1 = frame+value are heap-allocated */\n"
                "} TurDynFrame;\n\n");

            /* DV2: per-var storage, destructor, constructor, and binding pop */
            for (uint32_t i = 0; i < n_items; i++) {
                const Expr *e = items[i];
                if (e->kind != EX_DEFDYNAMIC) continue;
                DynVarEntry *entry = e->as.defdynamic_.entry;
                char *mname = mangle_dynvar_name(entry->name->name);
                const char *ctype = type_c_name(entry->value_type);
                buf_printf(&early_file, "static %s _dynvar_root_%s;\n", ctype, mname);
                buf_printf(&early_file, "static pthread_key_t _dynvar_key_%s;\n", mname);
                /* DV3: destructor walks chain and frees heap frames on thread exit. */
                buf_printf(&early_file,
                    "static void _dynvar_cleanup_%s(void *f) {\n"
                    "    TurDynFrame *frame = (TurDynFrame *)f;\n"
                    "    while (frame && frame->heap) {\n"
                    "        TurDynFrame *prev = frame->prev;\n"
                    "        free(frame->value);\n"
                    "        free(frame);\n"
                    "        frame = prev;\n"
                    "    }\n"
                    "}\n"
                    "__attribute__((constructor))\n"
                    "static void _dynvar_init_%s(void) {\n"
                    "    pthread_key_create(&_dynvar_key_%s, _dynvar_cleanup_%s);\n"
                    "}\n"
                    "static void _dynvar_pop_%s(TurDynFrame **fp) {\n"
                    "    pthread_setspecific(_dynvar_key_%s, (*fp)->prev);\n"
                    "}\n\n",
                    mname,
                    mname, mname, mname,
                    mname, mname);
                free(mname);
            }

            /* DV3: _TurDynSnap struct -- one field pair per dynamic var. */
            buf_puts(&early_file, "/* DV3: binding snapshot for spawn-conveying */\n");
            buf_puts(&early_file, "typedef struct {\n");
            for (uint32_t i = 0; i < n_items; i++) {
                const Expr *e = items[i];
                if (e->kind != EX_DEFDYNAMIC) continue;
                DynVarEntry *entry = e->as.defdynamic_.entry;
                char *mname = mangle_dynvar_name(entry->name->name);
                const char *ctype = type_c_name(entry->value_type);
                buf_printf(&early_file,
                    "    int has_%s; %s val_%s;\n", mname, ctype, mname);
                free(mname);
            }
            buf_puts(&early_file, "} _TurDynSnap;\n\n");

            /* DV3: _tur_binding_snapshot_capture -- copy top frame value for each var. */
            buf_puts(&early_file,
                "static _TurDynSnap *_tur_binding_snapshot_capture(void) {\n"
                "    _TurDynSnap *s = (_TurDynSnap *)calloc(1, sizeof(_TurDynSnap));\n"
                "    if (!s) { fprintf(stderr, \"tur: oom\\n\"); abort(); }\n");
            for (uint32_t i = 0; i < n_items; i++) {
                const Expr *e = items[i];
                if (e->kind != EX_DEFDYNAMIC) continue;
                DynVarEntry *entry = e->as.defdynamic_.entry;
                char *mname = mangle_dynvar_name(entry->name->name);
                const char *ctype = type_c_name(entry->value_type);
                buf_printf(&early_file,
                    "    { TurDynFrame *_f = (TurDynFrame *)pthread_getspecific(_dynvar_key_%s);\n"
                    "      if (_f) { s->has_%s = 1; s->val_%s = *(%s *)_f->value; } }\n",
                    mname, mname, mname, ctype);
                free(mname);
            }
            buf_puts(&early_file, "    return s;\n}\n\n");

            /* DV3: _tur_binding_snapshot_install -- push heap frames on the new thread. */
            buf_puts(&early_file,
                "static void _tur_binding_snapshot_install(_TurDynSnap *s) {\n");
            for (uint32_t i = 0; i < n_items; i++) {
                const Expr *e = items[i];
                if (e->kind != EX_DEFDYNAMIC) continue;
                DynVarEntry *entry = e->as.defdynamic_.entry;
                char *mname = mangle_dynvar_name(entry->name->name);
                const char *ctype = type_c_name(entry->value_type);
                buf_printf(&early_file,
                    "    if (s->has_%s) {\n"
                    "        %s *_v = (%s *)malloc(sizeof(%s));\n"
                    "        if (!_v) { fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                    "        *_v = s->val_%s;\n"
                    "        TurDynFrame *_fr = (TurDynFrame *)malloc(sizeof(TurDynFrame));\n"
                    "        if (!_fr) { free(_v); fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                    "        _fr->prev  = (TurDynFrame *)pthread_getspecific(_dynvar_key_%s);\n"
                    "        _fr->value = _v;\n"
                    "        _fr->heap  = 1;\n"
                    "        pthread_setspecific(_dynvar_key_%s, _fr);\n"
                    "    }\n",
                    mname, ctype, ctype, ctype, mname, mname, mname);
                free(mname);
            }
            buf_puts(&early_file, "}\n\n");

            /* DV3: convey arg + trampoline + spawn-conveying entry point.
             * These reference TurThreadState/TurThreadHandle which are emitted
             * earlier in the output (before early_file is appended). */
            buf_puts(&early_file,
                "typedef struct {\n"
                "    int64_t         closure;\n"
                "    TurThreadState *state;\n"
                "    _TurDynSnap    *snap;\n"
                "} _TurConveyArg;\n\n"
                "static void *_tur_convey_trampoline(void *raw) {\n"
                "    _TurConveyArg *a = (_TurConveyArg *)raw;\n"
                "    tur_current_thread_state = a->state;\n"
                "    _tur_binding_snapshot_install(a->snap);\n"
                "    free(a->snap);\n"
                "    void (*fn)(void) = (void (*)(void))(intptr_t)a->closure;\n"
                "    free(a);\n"
                "    fn();\n"
                "    return NULL;\n"
                "}\n\n"
                "static void *_tur_spawn_conveying(int64_t closure) {\n"
                "    _TurDynSnap *snap = _tur_binding_snapshot_capture();\n"
                "    TurThreadState *state = (TurThreadState *)calloc(1, sizeof(TurThreadState));\n"
                "    if (!state) { free(snap); fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                "    pthread_mutex_init(&state->cancel_mutex, NULL);\n"
                "    pthread_cond_init(&state->cancel_cond, NULL);\n"
                "    _TurConveyArg *arg = (_TurConveyArg *)malloc(sizeof(_TurConveyArg));\n"
                "    if (!arg) { free(state); free(snap); fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                "    arg->closure = closure;\n"
                "    arg->state   = state;\n"
                "    arg->snap    = snap;\n"
                "    TurThreadHandle *h = (TurThreadHandle *)malloc(sizeof(TurThreadHandle));\n"
                "    if (!h) { free(arg); free(state); free(snap); fprintf(stderr, \"tur: oom\\n\"); abort(); }\n"
                "    h->state = state;\n"
                "    int _rc = pthread_create(&h->tid, NULL, _tur_convey_trampoline, arg);\n"
                "    if (_rc != 0) {\n"
                "        free(h); free(arg); free(state); free(snap);\n"
                "        fprintf(stderr, \"tur: spawn-conveying: pthread_create failed (%d)\\n\", _rc);\n"
                "        abort();\n"
                "    }\n"
                "    return (void *)h;\n"
                "}\n\n");
        }
    }

    /* Pass 1: Emit forward declarations for all functions.
     * Written to fwd_decls buffer (emitted before pending_handler_fns in final
     * assembly) so that effect handler functions can call user-defined functions. */
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            /* Skip main - it's not called from other functions in the same file */
            if (strcmp(fd->binding->name->name, "main") == 0) continue;
            /* Phase M6: exported module functions don't need static on their
             * forward declaration — they already get external linkage in emit_fn_def
             * when separate_compilation is true. In single-file mode (the common
             * case here) all functions are still static. */
            if (!ctx.separate_compilation || !fd->binding->is_exported) {
                buf_puts(&fwd_decls, "static ");
            }
            if (e->type.kind == TY_FN) {
                TypeKind result = e->type.as.fn.result_kind;
                /* LT4: TY_STRUCT return types lower to the actual struct name in C.
                 * Avoid emit_type_from_kind(TY_STRUCT) here — it produces a Type with a
                 * zeroed def pointer; passing that large struct by value can trigger
                 * UBSan false positives in debug builds.  Instead emit the name
                 * directly from the fn_type's result_full_type if present, or fall
                 * back to "int64_t" for opaque/unresolved struct types. */
                if (result == TY_STRUCT) {
                    const struct Type *rft = e->type.as.fn.result_full_type;
                    if (rft && rft->kind == TY_STRUCT && rft->as.struct_.def &&
                        rft->as.struct_.def->name) {
                        buf_puts(&fwd_decls, rft->as.struct_.def->name);
                    } else {
                        buf_puts(&fwd_decls, "int64_t");
                    }
                } else {
                    buf_puts(&fwd_decls, type_c_name(emit_type_from_kind(result)));
                }
            } else {
                buf_puts(&fwd_decls, "void");
            }
            const char *fn_name = raw_name_for_binding(fd->binding);
            buf_printf(&fwd_decls, " %s(", fn_name);
            for (uint8_t j = 0; j < fd->n_params; j++) {
                if (j > 0) buf_puts(&fwd_decls, ", ");
                /* Phase HRT1: poly fn params use tur_poly_fn_t in signature */
                if (fd->params[j]->is_poly_fn) {
                    buf_puts(&fwd_decls, "tur_poly_fn_t");
                } else if (fd->param_types[j].kind == TY_FN) {
                    /* ER4: function-typed parameters are passed as int64_t. */
                    buf_puts(&fwd_decls, "int64_t");
                } else if (fd->param_types[j].kind == TY_STRUCT) {
                    /* LT4: struct params lower to the actual struct name. */
                    const StructDef *sd = fd->param_types[j].as.struct_.def;
                    if (sd && sd->name) {
                        buf_puts(&fwd_decls, sd->name);
                    } else {
                        buf_puts(&fwd_decls, "int64_t");
                    }
                } else {
                    buf_puts(&fwd_decls, type_c_name(fd->param_types[j]));
                }
            }
            buf_puts(&fwd_decls, ");\n");
            free((void*)fn_name);
        }
    }

    /* Phase M5: collect top-level EX_DEFER nodes (module-level defers). */
    uint32_t n_prog_defers = 0;
    for (uint32_t i = 0; i < n_items; i++) {
        if (items[i]->kind == EX_DEFER) n_prog_defers++;
    }
    const Expr **prog_defers = NULL;
    if (n_prog_defers > 0) {
        prog_defers = (const Expr **)malloc(n_prog_defers * sizeof(Expr *));
        uint32_t di = 0;
        for (uint32_t i = 0; i < n_items; i++) {
            if (items[i]->kind == EX_DEFER)
                prog_defers[di++] = items[i];
        }
    }

    /* Pass 2: collect all top-level defs and fn_defs. */
    for (uint32_t i = 0; i < n_items; i++) {
        const Expr *e = items[i];
        if (e->kind == EX_DEFER) {
            /* Phase M5: module-level defers handled after this pass. */
            continue;
        } else if (e->kind == EX_DEFDATA) {
            /* Phase G0: ADT typedefs and constructor functions already emitted in Pass 0 */
            continue;
        } else if (e->kind == EX_DEF) {
            /* Phase 11: skip struct typedefs — already emitted in Pass 0 */
            if (e->as.def_.struct_def) continue;
            char *bn = name_for_binding(&ctx, e->as.def_.binding);
            buf_printf(&file, "static %s %s;\n",
                       type_c_name(e->as.def_.binding->type), bn);
            if (e->as.def_.init) {
                char *iv = emit_value(&ctx, &body, e->as.def_.init);
                indent_buf(&body, ctx.indent);
                buf_printf(&body, "%s = %s;\n", bn, iv);
                free(iv);
            }
            free(bn);
        } else if (e->kind == EX_FN_DEF) {
            /* Emit function definition at file scope */
            emit_fn_def(&ctx, &file, e);
        } else if (e->kind == EX_EXTERN_C) {
            /* Emit extern-c declaration early (before handler functions) */
            ExternC *ec = e->as.extern_c_.ext;
            /* When HAMT lowering is active, hamt.h is already included and
             * declares all tur_hamt_* functions; skip conflicting extern decls. */
            /* Also suppress redeclarations of C stdlib functions already in the preamble. */
            /* Functions declared in the runtime preamble (emit_runtime_preamble) or
             * via headers always included (<stdio.h>, <stdlib.h>).
             * Suppress redeclarations to avoid conflicting-types errors. */
            static const char *preamble_decls[] = {
                /* explicit extern decls in preamble */
                "malloc","calloc","free","abort","atexit",
                "memset","memmove","memcpy","memcmp","strcmp","strlen","strcpy","strncpy","strcat","strncat","strstr","strchr","strrchr","strdup",
                /* <stdio.h> */
                "printf","fprintf","sprintf","snprintf","scanf","sscanf","fscanf",
                "fopen","fclose","fread","fwrite","fseek","ftell","fflush","rewind",
                "puts","putchar","getchar","putc","getc","fputc","fgetc","fputs","fgets",
                "perror","clearerr","feof","ferror","remove","rename","tmpfile",
                /* <stdlib.h> */
                "exit","getenv","putenv","system","rand","srand","bsearch","qsort",
                "atoi","atol","atof","strtol","strtoul","strtod",
                NULL
            };
            bool suppress_ec = false;
            if (g_needs_hamt && strncmp(ec->c_name->name, "tur_hamt_", 9) == 0) {
                suppress_ec = true; /* Suppress: declared by #include "hamt.h" */
            } else {
                for (int si = 0; preamble_decls[si]; si++) {
                    if (strcmp(ec->c_name->name, preamble_decls[si]) == 0) {
                        suppress_ec = true; /* Suppress: already in preamble */
                        break;
                    }
                }
            }
            if (!suppress_ec) {
            char *ec_mangled = mangle_field_name(ec->c_name->name);
            buf_printf(&extern_decls, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec_mangled);
            free(ec_mangled);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(&extern_decls, ", ");
                buf_printf(&extern_decls, "%s", type_c_name(ec->param_types[j]));
            }
            buf_puts(&extern_decls, ");\n");
            }
        } else if (e->kind == EX_DEFDYNAMIC) {
            /* DV2: initialize the root value in main() body. */
            DynVarEntry *entry = e->as.defdynamic_.entry;
            char *mname = mangle_dynvar_name(entry->name->name);
            char *rv = emit_value(&ctx, &body, e->as.defdynamic_.root_expr);
            indent_buf(&body, ctx.indent);
            buf_printf(&body, "_dynvar_root_%s = %s;\n", mname, rv);
            free(rv);
            free(mname);
        } else {
            emit_stmt(&ctx, &body, e);
        }
    }

    /* Phase M5: emit module-level defer thunks + atexit constructor. */
    if (n_prog_defers > 0) {
        buf_puts(&file, "\n/* Phase M5: module-level defers */\n");
        for (uint32_t i = 0; i < n_prog_defers; i++) {
            buf_printf(&file, "static void __module_defer_%u(void) {\n", i);
            Buf thunk_body; buf_init(&thunk_body);
            const char *saved_frame = ctx.frame_var;
            ctx.frame_var = NULL;
            ctx.indent = 4;
            emit_stmt(&ctx, &thunk_body, prog_defers[i]->as.defer_.body);
            ctx.frame_var = saved_frame;
            buf_write(&file, thunk_body.data, thunk_body.len);
            buf_free(&thunk_body);
            buf_puts(&file, "}\n");
        }
        buf_puts(&file,
            "__attribute__((constructor))\n"
            "static void __module_defers_init(void) {\n");
        for (uint32_t i = 0; i < n_prog_defers; i++) {
            buf_printf(&file, "    atexit(__module_defer_%u);\n", i);
        }
        buf_puts(&file, "}\n");
        free(prog_defers);
    }

    /* Final assembly. */
    buf_puts(out, "/* generated by tur (phase 2) */\n");
    /* Feature-test macro: must precede every #include so glibc exposes POSIX
     * declarations (clock_gettime, nanosleep, ...) used by the emitted runtime
     * even under a strict -std=c99 compile. No-op on Apple libc. */
    buf_puts(out, "#define _DEFAULT_SOURCE 1\n");
    /* Suppress warnings for unused helpers that are part of the runtime preamble
     * but not exercised by every program.  Both GCC and Clang honour these. */
    buf_puts(out, "#pragma GCC diagnostic ignored \"-Wunused-function\"\n");
    buf_puts(out, "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n");
    buf_puts(out, "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n");
    /* Phase P3: HAMT lowering - include HAMT header when needed */
    if (g_needs_hamt) {
        buf_puts(out, "#include \"hamt.h\"\n");
    }
    /* Phase T24: BSD networking headers (sys/socket.h, netinet/in.h, arpa/inet.h)
     * MUST come before ucontext.h.  On macOS, #define _XOPEN_SOURCE 700 suppresses
     * BSD extensions; once any header is processed with _XOPEN_SOURCE active, its
     * include guard prevents re-inclusion with BSD extensions enabled.  Including
     * networking headers first (without _XOPEN_SOURCE) lets them define INADDR_*,
     * sockaddr_in, etc., before ucontext.h locks in the POSIX-strict feature set. */
    buf_puts(out, "#include <sys/select.h>\n");
    buf_puts(out, "#include <sys/socket.h>\n");
    buf_puts(out, "#include <netinet/in.h>\n");
    buf_puts(out, "#include <arpa/inet.h>\n");
    /* Phase T21: ucontext.h must come before setjmp.h and pthread.h.
     * On macOS, setjmp.h indirectly includes a minimal ucontext.h without
     * _XOPEN_SOURCE, locking in the small 56-byte ucontext_t via include guards.
     * FiberBlock embeds two ucontext_t fields and needs the full 880-byte layout. */
    buf_puts(out, "#define _XOPEN_SOURCE 700\n");
    buf_puts(out, "#include <ucontext.h>\n");
    buf_puts(out, "#undef _XOPEN_SOURCE\n");
    buf_puts(out, "#include <setjmp.h>\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    /* Phase T19: Thread primitives - pthread on all supported platforms */
    buf_puts(out, "#include <pthread.h>\n");
    /* Phase 20-21: Software Transactional Memory */
    buf_puts(out, "#include <stdlib.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    buf_puts(out, "#include <string.h>\n");
    /* Phase X3: Set literal runtime — sorted-array representation (Option A, v1) */
    buf_puts(out, "/* Phase X3: tur_set_t — sorted int64_t array */\n");
    buf_puts(out, "typedef struct { int64_t *items; uint32_t n; } tur_set_t;\n");
    buf_puts(out, "static int __tur_set_cmp(const void *a, const void *b) {\n");
    buf_puts(out, "    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;\n");
    buf_puts(out, "    return (x > y) - (x < y);\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_from_items(uint32_t n, int64_t *src) {\n");
    buf_puts(out, "    tur_set_t *s = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "    s->items = n ? (int64_t *)malloc(n * sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "    if (n) memcpy(s->items, src, n * sizeof(int64_t));\n");
    buf_puts(out, "    if (n > 1) qsort(s->items, n, sizeof(int64_t), __tur_set_cmp);\n");
    buf_puts(out, "    uint32_t k = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < n; i++)\n");
    buf_puts(out, "        if (k == 0 || s->items[k-1] != s->items[i]) s->items[k++] = s->items[i];\n");
    buf_puts(out, "    s->n = k;\n");
    buf_puts(out, "    return s;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static bool tur_set_member(tur_set_t *s, int64_t x) {\n");
    buf_puts(out, "    if (!s || !s->n) return false;\n");
    buf_puts(out, "    int lo = 0, hi = (int)s->n - 1;\n");
    buf_puts(out, "    while (lo <= hi) { int mid = (lo+hi)/2;\n");
    buf_puts(out, "        if (s->items[mid] == x) return true;\n");
    buf_puts(out, "        if (s->items[mid] < x) lo = mid+1; else hi = mid-1; }\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static int64_t tur_set_count(tur_set_t *s) { return s ? (int64_t)s->n : 0; }\n");
    buf_puts(out, "static tur_set_t *tur_set_add(tur_set_t *s, int64_t x) {\n");
    buf_puts(out, "    if (tur_set_member(s, x)) {\n");
    buf_puts(out, "        tur_set_t *r = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "        r->n = s->n; r->items = s->n ? (int64_t *)malloc(s->n*sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "        if (s->n) memcpy(r->items, s->items, s->n*sizeof(int64_t));\n");
    buf_puts(out, "        return r;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_set_t *r = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "    r->n = (s ? s->n : 0) + 1;\n");
    buf_puts(out, "    r->items = (int64_t *)malloc(r->n * sizeof(int64_t));\n");
    buf_puts(out, "    uint32_t pos = 0, base = s ? s->n : 0;\n");
    buf_puts(out, "    while (pos < base && s->items[pos] < x) pos++;\n");
    buf_puts(out, "    if (s && pos > 0) memcpy(r->items, s->items, pos*sizeof(int64_t));\n");
    buf_puts(out, "    r->items[pos] = x;\n");
    buf_puts(out, "    if (s && pos < base) memcpy(r->items+pos+1, s->items+pos, (base-pos)*sizeof(int64_t));\n");
    buf_puts(out, "    return r;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_remove(tur_set_t *s, int64_t x) {\n");
    buf_puts(out, "    if (!s || !s->n || !tur_set_member(s, x)) {\n");
    buf_puts(out, "        tur_set_t *r = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "        r->n = s ? s->n : 0;\n");
    buf_puts(out, "        r->items = r->n ? (int64_t *)malloc(r->n*sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "        if (r->n) memcpy(r->items, s->items, r->n*sizeof(int64_t));\n");
    buf_puts(out, "        return r;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_set_t *r = (tur_set_t *)malloc(sizeof(tur_set_t));\n");
    buf_puts(out, "    r->items = s->n > 1 ? (int64_t *)malloc((s->n-1)*sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "    uint32_t k = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < s->n; i++) if (s->items[i] != x) r->items[k++] = s->items[i];\n");
    buf_puts(out, "    r->n = k; return r;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_union(tur_set_t *a, tur_set_t *b) {\n");
    buf_puts(out, "    uint32_t na = a?a->n:0, nb = b?b->n:0, cap = na+nb;\n");
    buf_puts(out, "    int64_t *tmp = cap ? (int64_t *)malloc(cap*sizeof(int64_t)) : NULL;\n");
    buf_puts(out, "    if (a) memcpy(tmp, a->items, na*sizeof(int64_t));\n");
    buf_puts(out, "    if (b) memcpy(tmp+na, b->items, nb*sizeof(int64_t));\n");
    buf_puts(out, "    return tur_set_from_items(cap, tmp);\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_intersection(tur_set_t *a, tur_set_t *b) {\n");
    buf_puts(out, "    if (!a || !b || !a->n || !b->n) return tur_set_from_items(0, NULL);\n");
    buf_puts(out, "    int64_t *tmp = (int64_t *)malloc(a->n*sizeof(int64_t));\n");
    buf_puts(out, "    uint32_t k = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < a->n; i++) if (tur_set_member(b, a->items[i])) tmp[k++] = a->items[i];\n");
    buf_puts(out, "    tur_set_t *r = tur_set_from_items(k, tmp); free(tmp); return r;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static tur_set_t *tur_set_difference(tur_set_t *a, tur_set_t *b) {\n");
    buf_puts(out, "    if (!a || !a->n) return tur_set_from_items(0, NULL);\n");
    buf_puts(out, "    int64_t *tmp = (int64_t *)malloc(a->n*sizeof(int64_t));\n");
    buf_puts(out, "    uint32_t k = 0;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < a->n; i++) if (!tur_set_member(b, a->items[i])) tmp[k++] = a->items[i];\n");
    buf_puts(out, "    tur_set_t *r = tur_set_from_items(k, tmp); free(tmp); return r;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_set_free(tur_set_t *s) { if (s) { free(s->items); free(s); } }\n");
    /* Phase HRT1: rank-2 polymorphic function type.
     * tur_poly_fn_t is a generic closure: a function pointer paired with an env pointer.
     * Used for (forall [a] (-> a a))-style rank-2 parameters. */
    buf_puts(out, "/* Phase HRT1: rank-2 polymorphic function type */\n");
    buf_puts(out, "typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;\n");
    /* ET3: handler runtime type.
     * tur_handler_t is a handler value: an env pointer plus a dispatch function.
     * The dispatch function receives: env, n_args, value, continuation-as-int64_t. */
    buf_puts(out, "/* ET3: algebraic effect handler runtime type */\n");
    buf_puts(out, "typedef struct { void *env; int64_t (*fn)(int64_t *, int, int64_t, void *); } tur_handler_t;\n");
    /* IT4: Tagged union runtime representation.
     * tur_tagged_t carries a discriminant tag and a 64-bit payload.
     * Used for (A | B) union types and the 'any' top type. */
    buf_puts(out, "/* IT4: tagged union runtime representation */\n");
    buf_puts(out, "typedef struct { int64_t tag; int64_t val; } tur_tagged_t;\n");
    buf_puts(out, "#define TUR_TAG(t, v)  ((tur_tagged_t){(int64_t)(t), (int64_t)(v)})\n");
    buf_puts(out, "#define TUR_UNTAG(x)   ((x).val)\n");
    buf_puts(out, "#define TUR_GETTAG(x)  ((x).tag)\n");
    /* IT4: (type-of x) helper — maps TypeKind tag to a cstr type name. */
    buf_puts(out, "static const char *__tur_any_type_name(int64_t tag) {\n");
    buf_puts(out, "    switch (tag) {\n");
    buf_puts(out, "        case  1: return \"nil\";\n");
    buf_puts(out, "        case  2: return \"bool\";\n");
    buf_puts(out, "        case  3: return \"int\";\n");
    buf_puts(out, "        case  4: return \"float\";\n");
    buf_puts(out, "        case  5: return \"cstr\";\n");
    buf_puts(out, "        case  6: return \"ptr\";\n");
    buf_puts(out, "        default: return \"unknown\";\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n");
    /* Phase HRT2: existential type — opaque void* wrapping any boxed value */
    buf_puts(out, "/* Phase HRT2: existential type (opaque void* box) */\n");
    buf_puts(out, "typedef void * tur_exists_t;\n");
    /* Inline STM runtime - Phase 21 with per-TVar locking */
    buf_puts(out, "/* STM types (Phase 21) */\n");
    buf_puts(out, "typedef void *(*stm_fn_t)(void *env);\n");
    buf_puts(out, "typedef struct TVar { void *value; uint64_t version; pthread_mutex_t lock; pthread_cond_t cond; } TVar;\n");
    buf_puts(out, "typedef struct STM_Transaction STM_Transaction;\n");
    buf_puts(out, "struct STM_Transaction {\n");
    buf_puts(out, "    TVar *read_set[256];\n");
    buf_puts(out, "    uint64_t read_versions[256];\n");
    buf_puts(out, "    int read_count;\n");
    buf_puts(out, "    TVar *write_set[128];\n");
    buf_puts(out, "    void *new_values[128];\n");
    buf_puts(out, "    int write_count;\n");
    buf_puts(out, "    bool retry_requested;\n");
    buf_puts(out, "    bool aborted;\n");
    buf_puts(out, "    bool committed;\n");
    buf_puts(out, "};\n");
    buf_puts(out, "static __thread STM_Transaction *__stm_current_tx = NULL;\n");
    buf_puts(out, "STM_Transaction *tur_stm_current_tx(void) { return __stm_current_tx; }\n");
    buf_puts(out, "void tur_stm_set_current_tx(STM_Transaction *tx) { __stm_current_tx = tx; }\n");
    /* Lock ordering helper */
    buf_puts(out, "static int __stm_lock_cmp(const void *a, const void *b) {\n");
    buf_puts(out, "    const TVar *tv_a = *(const TVar **)a;\n");
    buf_puts(out, "    const TVar *tv_b = *(const TVar **)b;\n");
    buf_puts(out, "    if (tv_a < tv_b) return -1;\n");
    buf_puts(out, "    if (tv_a > tv_b) return 1;\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "STM_Transaction *tur_stm_new_transaction(void) {\n");
    buf_puts(out, "    STM_Transaction *tx = calloc(1, sizeof(STM_Transaction));\n");
    buf_puts(out, "    return tx;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "TVar *tur_tvar_new(void *type, void *initial_value) {\n");
    buf_puts(out, "    (void)type; /* unused in emitted code */\n");
    buf_puts(out, "    TVar *tv = malloc(sizeof(TVar));\n");
    buf_puts(out, "    tv->value = initial_value; tv->version = 1;\n");
    buf_puts(out, "    pthread_mutex_init(&tv->lock, NULL);\n");
    buf_puts(out, "    pthread_cond_init(&tv->cond, NULL);\n");
    buf_puts(out, "    return tv;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "void *tur_tvar_read(STM_Transaction *tx, TVar *tv) {\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        if (tx->write_set[i] == tv) return tx->new_values[i];\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (tx->read_count < 256) {\n");
    buf_puts(out, "        for (int i = 0; i < tx->read_count; i++) {\n");
    buf_puts(out, "            if (tx->read_set[i] == tv) break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        tx->read_set[tx->read_count] = tv;\n");
    buf_puts(out, "        tx->read_versions[tx->read_count++] = tv->version;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return tv->value;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "void tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value) {\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        if (tx->write_set[i] == tv) { tx->new_values[i] = value; return; }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Remove from read set if present */\n");
    buf_puts(out, "    for (int i = 0; i < tx->read_count; i++) {\n");
    buf_puts(out, "        if (tx->read_set[i] == tv) {\n");
    buf_puts(out, "            for (int j = i; j < tx->read_count - 1; j++) {\n");
    buf_puts(out, "                tx->read_set[j] = tx->read_set[j+1];\n");
    buf_puts(out, "                tx->read_versions[j] = tx->read_versions[j+1];\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            tx->read_count--;\n");
    buf_puts(out, "            break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (tx->write_count < 128) {\n");
    buf_puts(out, "        tx->write_set[tx->write_count] = tv;\n");
    buf_puts(out, "        tx->new_values[tx->write_count++] = value;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n");
    /* Validation */
    buf_puts(out, "static bool __tur_stm_validate(STM_Transaction *tx) {\n");
    buf_puts(out, "    for (int i = 0; i < tx->read_count; i++) {\n");
    buf_puts(out, "        if (tx->read_set[i]->version != tx->read_versions[i]) return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return true;\n");
    buf_puts(out, "}\n");
    /* Commit with per-TVar locking (Phase 21) */
    buf_puts(out, "bool tur_stm_commit(STM_Transaction *tx) {\n");
    buf_puts(out, "    /* Sort write set by address for lock ordering */\n");
    buf_puts(out, "    qsort(tx->write_set, tx->write_count, sizeof(TVar *), __stm_lock_cmp);\n");
    buf_puts(out, "    /* Acquire locks in address order */\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        pthread_mutex_lock(&tx->write_set[i]->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Validate */\n");
    buf_puts(out, "    if (!__tur_stm_validate(tx)) {\n");
    buf_puts(out, "        for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "            pthread_mutex_unlock(&tx->write_set[i]->lock);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Apply writes */\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        tx->write_set[i]->value = tx->new_values[i];\n");
    buf_puts(out, "        tx->write_set[i]->version++;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Notify waiters */\n");
    buf_puts(out, "    for (int i = 0; i < tx->write_count; i++) {\n");
    buf_puts(out, "        pthread_cond_broadcast(&tx->write_set[i]->cond);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tx->committed = true;\n");
    buf_puts(out, "    /* Release locks in reverse order */\n");
    buf_puts(out, "    for (int i = tx->write_count - 1; i >= 0; i--) {\n");
    buf_puts(out, "        pthread_mutex_unlock(&tx->write_set[i]->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return true;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "void tur_stm_retry(STM_Transaction *tx) { tx->retry_requested = true; }\n");
    buf_puts(out, "void tur_stm_check(bool condition) { if (!condition) tur_stm_retry(tur_stm_current_tx()); }\n");
    buf_puts(out, "/* Retry with blocking on condition variables (Phase 21) */\n");
    buf_puts(out, "static bool __tur_stm_should_retry(STM_Transaction *tx) {\n");
    buf_puts(out, "    if (tx->retry_requested) return true;\n");
    buf_puts(out, "    if (tx->aborted) return true;\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "void *tur_atomically(void *(*fn)(void *), void *env) {\n");
    buf_puts(out, "    STM_Transaction *tx = tur_stm_new_transaction();\n");
    buf_puts(out, "    STM_Transaction *prev = tur_stm_current_tx();\n");
    buf_puts(out, "    while (1) {\n");
    buf_puts(out, "        tx->retry_requested = false;\n");
    buf_puts(out, "        tx->aborted = false;\n");
    buf_puts(out, "        tx->read_count = 0;\n");
    buf_puts(out, "        tx->write_count = 0;\n");
    buf_puts(out, "        tur_stm_set_current_tx(tx);\n");
    buf_puts(out, "        fn(env);\n");
    buf_puts(out, "        if (__tur_stm_should_retry(tx)) {\n");
    buf_puts(out, "            tur_stm_set_current_tx(prev);\n");
    buf_puts(out, "            /* Block on first read TVar's condition variable if retry requested */\n");
    buf_puts(out, "            if (tx->retry_requested && tx->read_count > 0) {\n");
    buf_puts(out, "                TVar *tv = tx->read_set[0];\n");
    buf_puts(out, "                pthread_mutex_lock(&tv->lock);\n");
    buf_puts(out, "                while (tx->retry_requested) {\n");
    buf_puts(out, "                    pthread_cond_wait(&tv->cond, &tv->lock);\n");
    buf_puts(out, "                }\n");
    buf_puts(out, "                pthread_mutex_unlock(&tv->lock);\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            continue;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        if (tur_stm_commit(tx)) {\n");
    buf_puts(out, "            tur_stm_set_current_tx(prev);\n");
    buf_puts(out, "            void *ret = tx->write_count > 0 ? tx->new_values[tx->write_count - 1] : NULL;\n");
    buf_puts(out, "            free(tx);\n");
    buf_puts(out, "            return ret;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* Commit failed - retry */\n");
    buf_puts(out, "        tur_stm_set_current_tx(prev);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n");
    /* Phase T21: ucontext.h was moved to the very top (before setjmp.h/pthread.h)
     * to prevent macOS include-guard aliasing of the small 56-byte ucontext_t.
     * Nothing to emit here any more. */
    /* Phase 5/9/M5/19: stdlib.h and string.h are included above; no need for
     * explicit extern declarations of malloc/calloc/free/abort/atexit/memset/
     * memmove/memcpy/strcmp — they are provided by the standard headers. */
    /* Phase T24: Headers for timer wheel, async I/O, and networking.
     * sys/select.h, sys/socket.h, netinet/in.h, arpa/inet.h were moved to the
     * very top (before ucontext.h) to prevent BSD-extension suppression. */
    buf_puts(out, "#include <time.h>\n");
    buf_puts(out, "#include <unistd.h>\n");
    buf_puts(out, "#include <fcntl.h>\n");
    buf_puts(out, "#include <errno.h>\n");
    buf_puts(out, "\n");
    /* Phase 7 follow-up: minimal in-process test registry for stdlib/test.tur. */
    buf_puts(out, "#define TUR_TEST_REGISTRY_MAX 1024\n");
    /* Phase B5: backtrack depth cap (0 = unlimited) */
    buf_printf(out, "#define BACKTRACK_DEPTH_DEFAULT %lld\n", (long long)g_backtrack_depth);
    buf_puts(out, "typedef int64_t (*tur_test_callback_t)(void);\n");
    buf_puts(out, "static const char *tur_test_registry_names[TUR_TEST_REGISTRY_MAX];\n");
    buf_puts(out, "static tur_test_callback_t tur_test_registry_fns[TUR_TEST_REGISTRY_MAX];\n");
    buf_puts(out, "static int64_t tur_test_registry_count = 0;\n\n");
    buf_puts(out, "int64_t tur_test_register(const char *name, void *test_fn) {\n");
    buf_puts(out, "    if (!test_fn) return 0;\n");
    buf_puts(out, "    if (tur_test_registry_count >= TUR_TEST_REGISTRY_MAX) return 0;\n");
    buf_puts(out, "    tur_test_registry_names[tur_test_registry_count] = name ? name : \"<unnamed>\";\n");
    buf_puts(out, "    tur_test_registry_fns[tur_test_registry_count] = (tur_test_callback_t)test_fn;\n");
    buf_puts(out, "    tur_test_registry_count++;\n");
    buf_puts(out, "    return 1;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "int64_t tur_test_run_all(void) {\n");
    buf_puts(out, "    int64_t passed = 0;\n");
    buf_puts(out, "    int64_t failed = 0;\n");
    buf_puts(out, "    for (int64_t i = 0; i < tur_test_registry_count; i++) {\n");
    buf_puts(out, "        tur_test_callback_t fn = tur_test_registry_fns[i];\n");
    buf_puts(out, "        int64_t rc = fn ? fn() : 0;\n");
    buf_puts(out, "        if (rc == 1) {\n");
    buf_puts(out, "            putchar('.');\n");
    buf_puts(out, "            putchar('\\n');\n");
    buf_puts(out, "            passed++;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            putchar('F');\n");
    buf_puts(out, "            putchar('\\n');\n");
    buf_puts(out, "            printf(\"%s\\n\", tur_test_registry_names[i]);\n");
    buf_puts(out, "            failed++;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    printf(\"summary: %lld passed, %lld failed\\n\",\n");
    buf_puts(out, "           (long long)passed, (long long)failed);\n");
    buf_puts(out, "    return failed == 0 ? 0 : 1;\n");
    buf_puts(out, "}\n\n");
    /* Phase 4 v1 lowering: emit tur_frame inline from runtime.h */
    buf_puts(out, "/* tur_frame - phase 4 v1 lowering (from runtime.h) */\n");
    buf_puts(out, "typedef void (*defer_fn_t)(void *env);\n");
    buf_puts(out, "#define TUR_FRAME_MAX_DEFERS 32\n\n");
    buf_puts(out, "typedef struct tur_frame {\n");
    buf_puts(out, "    defer_fn_t defers[TUR_FRAME_MAX_DEFERS];\n");
    buf_puts(out, "    void *envs[TUR_FRAME_MAX_DEFERS];\n");
    buf_puts(out, "    int n;\n");
    buf_puts(out, "    struct tur_frame *parent;\n");
    buf_puts(out, "    bool may_capture;\n");
    buf_puts(out, "} tur_frame;\n\n");
    buf_puts(out, "static inline void tur_frame_init(tur_frame *f, tur_frame *parent) {\n");
    buf_puts(out, "    f->n = 0; f->parent = parent; f->may_capture = false;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static inline int tur_frame_push_defer(tur_frame *f, defer_fn_t thunk, void *env) {\n");
    buf_puts(out, "    if (f->n >= TUR_FRAME_MAX_DEFERS) return -1;\n");
    buf_puts(out, "    f->defers[f->n] = thunk;\n");
    buf_puts(out, "    f->envs[f->n] = env;\n");
    buf_puts(out, "    f->n++;\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_frame_fire_lifo(tur_frame *f) {\n");
    buf_puts(out, "    for (int i = f->n - 1; i >= 0; i--) f->defers[i](f->envs[i]);\n");
    buf_puts(out, "    f->n = 0;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_frame_fire_chain(tur_frame *f) {\n");
    buf_puts(out, "    tur_frame *frames[64];\n");
    buf_puts(out, "    int n_frames = 0;\n");
    buf_puts(out, "    for (tur_frame *cur = f; cur != NULL && n_frames < 64; cur = cur->parent) {\n");
    buf_puts(out, "        frames[n_frames++] = cur;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    for (int i = n_frames - 1; i >= 0; i--) {\n");
    buf_puts(out, "        tur_frame_fire_lifo(frames[i]);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    
    /* Phase R2: tur_panic - integrated with defer chain */
    /* Phase R6: Add g_panic_trace flag for scope chain printing */
    buf_puts(out, "/* Phase R2/R6: tur_panic */\n");
    buf_puts(out, "static int tur_panic_in_progress = 0;\n");
    buf_puts(out, "static tur_frame *global_panic_frame = NULL;\n");
    buf_puts(out, "static int g_panic_trace = 0;  /* Set by compiler when --panic-trace is used */\n");
    /* CLI-ARGS: g_tur_args holds the *args* list (linked list of argv strings, built in main). */
    buf_puts(out, "static int64_t g_tur_args = 0;  /* *args*: CLI arguments as list of :cstr (set in main) */\n");
    buf_puts(out, "static void tur_panic_set_frame(tur_frame *f) {\n");
    buf_puts(out, "    global_panic_frame = f;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_panic_print_scope_chain(void) {\n");
    buf_puts(out, "    if (!g_panic_trace || !global_panic_frame) return;\n");
    buf_puts(out, "    fprintf(stderr, \"  scope chain:\\n\");\n");
    buf_puts(out, "    tur_frame *frames[64];\n");
    buf_puts(out, "    int n_frames = 0;\n");
    buf_puts(out, "    for (tur_frame *cur = global_panic_frame; cur != NULL && n_frames < 64; cur = cur->parent) {\n");
    buf_puts(out, "        frames[n_frames++] = cur;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    for (int i = 0; i < n_frames; i++) {\n");
    buf_puts(out, "        fprintf(stderr, \"    at frame %p (parent: %p, n_defers: %d)\\n\",\n");
    buf_puts(out, "                (void*)frames[i], (void*)frames[i]->parent, frames[i]->n);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_panic(const char *msg) {\n");
    buf_puts(out, "    if (tur_panic_in_progress) {\n");
    buf_puts(out, "        fprintf(stderr, \"double panic: aborting\\n\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_panic_in_progress = 1;\n");
    buf_puts(out, "    fprintf(stderr, \"panic at %s:%d: %s\\n\", __FILE__, __LINE__, msg ? msg : \"(no message)\");\n");
    buf_puts(out, "    tur_panic_print_scope_chain();\n");
    buf_puts(out, "    if (global_panic_frame) {\n");
    buf_puts(out, "        tur_frame_fire_chain(global_panic_frame);\n");
    buf_puts(out, "    }\n");
    /* Flush buffered output (defers fired above may have printed) before
     * abort(), which does not flush stdio streams. */
    buf_puts(out, "    fflush(NULL);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");

    /* Phase R5: tur_panic_abort for #[no-unwind] */
    buf_puts(out, "/* Phase R5: tur_panic_abort - no unwinding, immediate abort */\n");
    buf_puts(out, "static void tur_panic_abort(const char *msg) {\n");
    buf_puts(out, "    fprintf(stderr, \"panic (no unwind): %s\\n\", msg ? msg : \"(no message)\");\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");

    /* Phase R2: Panic with typed payload */
    /* tur_panic_with is forward-declared here; its body is emitted after
     * FiberBlock in Phase T21 so it can dereference tur_current_fiber. */
    buf_puts(out, "static void tur_panic_with(int type_tag, void *payload, const char *file, int line);\n\n");
    buf_puts(out, "/* Phase R2: tur_panic_with types */\n");
    buf_puts(out, "typedef struct tur_panic_payload tur_panic_payload;\n");
    buf_puts(out, "struct tur_panic_payload {\n");
    buf_puts(out, "    int type_tag;\n");
    buf_puts(out, "    void *value;\n");
    buf_puts(out, "    const char *file;\n");
    buf_puts(out, "    int line;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static tur_panic_payload *global_panic_payload = NULL;\n");
    buf_puts(out, "static jmp_buf global_panic_jmpbuf;\n");
    buf_puts(out, "static int global_panic_jmpbuf_valid = 0;\n\n");
    buf_puts(out, "static tur_panic_payload *panic_payload_new(int type_tag, void *payload, const char *file, int line) {\n");
    buf_puts(out, "    tur_panic_payload *p = (tur_panic_payload *)malloc(sizeof(tur_panic_payload));\n");
    buf_puts(out, "    if (!p) { fprintf(stderr, \"panic: oom\\n\"); abort(); }\n");
    buf_puts(out, "    p->type_tag = type_tag; p->value = payload; p->file = file; p->line = line;\n");
    buf_puts(out, "    return p;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void panic_payload_free(tur_panic_payload *p) {\n");
    buf_puts(out, "    if (p) { free(p->value); free(p); }\n");
    buf_puts(out, "}\n\n");
    /* Phase R2: Panic payload accessors */
    buf_puts(out, "static int tur_panic_payload_type(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->type_tag : 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void *tur_panic_payload_value(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->value : NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static const char *tur_panic_payload_file(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->file : NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int tur_panic_payload_line(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->line : 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void *tur_panic_payload_downcast(tur_panic_payload *p, int target_type) {\n");
    buf_puts(out, "    if (!p || p->type_tag != target_type) return NULL;\n");
    buf_puts(out, "    return p->value;\n");
    buf_puts(out, "}\n\n");
    /* Phase R2: catch-unwind and catch-panic-of */
    buf_puts(out, "/* Phase R2: catch-unwind/catch-panic-of */\n");
    buf_puts(out, "typedef enum { TUR_RESULT_OK, TUR_RESULT_ERR } tur_result_tag;\n");
    buf_puts(out, "typedef struct tur_result tur_result;\n");
    buf_puts(out, "struct tur_result {\n");
    buf_puts(out, "    tur_result_tag tag;\n");
    buf_puts(out, "    union { int64_t ok_val; void *ok_ptr; tur_panic_payload *err; } u;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "typedef void (*tur_thunk_fn)(void *env, tur_result *out);\n\n");
    buf_puts(out, "static bool tur_catch_unwind(tur_thunk_fn thunk, void *env, tur_result *out) {\n");
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    global_panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "    if (setjmp(global_panic_jmpbuf) == 0) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        tur_panic_in_progress = 0;\n");
    buf_puts(out, "        out->tag = TUR_RESULT_ERR;\n");
    buf_puts(out, "        out->u.err = global_panic_payload;\n");
    buf_puts(out, "        global_panic_payload = NULL;\n");
    buf_puts(out, "        return true;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static bool tur_catch_panic_of(int expected_type, tur_thunk_fn thunk, void *env, tur_result *out) {\n");
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    global_panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "    if (setjmp(global_panic_jmpbuf) == 0) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        tur_panic_in_progress = 0;\n");
    buf_puts(out, "        if (global_panic_payload && global_panic_payload->type_tag == expected_type) {\n");
    buf_puts(out, "            out->tag = TUR_RESULT_ERR;\n");
    buf_puts(out, "            out->u.err = global_panic_payload;\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "            return true;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            /* Type mismatch - re-panic */\n");
    buf_puts(out, "            tur_panic_with(global_panic_payload->type_tag, global_panic_payload->value,\n");
    buf_puts(out, "                           global_panic_payload->file, global_panic_payload->line);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    /* Phase B2 / MS1: Cloneable continuation runtime (inline in generated C).
     * Emitted when the program uses cloneable-shift/reset OR any ^multishot handler
     * (which uses tur_cloneable_cont wrappers + tur_continuation_snapshot). */
    if (cps_expr_contains_cloneable_shift(program) || expr_has_multishot_handler(program)) {
    buf_puts(out, "/* Phase B2: Cloneable continuation runtime */\n");
    buf_puts(out, "typedef struct tur_cloneable_cont tur_cloneable_cont;\n");
    buf_puts(out, "struct tur_cloneable_cont {\n");
    buf_puts(out, "    int64_t (*cont_fn)(void *env, int64_t value);\n");
    buf_puts(out, "    void *env;\n");
    buf_puts(out, "    void *(*clone_env)(const void *env);\n");
    buf_puts(out, "    void (*drop_env)(void *env);\n");
    buf_puts(out, "};\n\n");
    /* Internal allocator — takes explicit function pointers */
    buf_puts(out, "static tur_cloneable_cont *tur_cloneable_cont_alloc(\n");
    buf_puts(out, "    int64_t (*cont_fn)(void *, int64_t),\n");
    buf_puts(out, "    void *env,\n");
    buf_puts(out, "    void *(*clone_env)(const void *),\n");
    buf_puts(out, "    void (*drop_env)(void *)) {\n");
    buf_puts(out, "    tur_cloneable_cont *c = malloc(sizeof(tur_cloneable_cont));\n");
    buf_puts(out, "    if (!c) abort();\n");
    buf_puts(out, "    c->cont_fn = cont_fn; c->env = env;\n");
    buf_puts(out, "    c->clone_env = clone_env; c->drop_env = drop_env;\n");
    buf_puts(out, "    return c;\n");
    buf_puts(out, "}\n\n");
    /* User-facing: resume — cont is an opaque int64_t (pointer cast) */
    buf_puts(out, "static int64_t tur_cloneable_cont_resume(int64_t cont_int, int64_t value) {\n");
    buf_puts(out, "    tur_cloneable_cont *cont = (tur_cloneable_cont *)(intptr_t)cont_int;\n");
    buf_puts(out, "    if (!cont || !cont->cont_fn) abort();\n");
    buf_puts(out, "    return cont->cont_fn(cont->env, value);\n");
    buf_puts(out, "}\n\n");
    /* User-facing: clone — deep-copy env via clone_env function pointer */
    buf_puts(out, "static int64_t tur_cloneable_cont_clone(int64_t cont_int) {\n");
    buf_puts(out, "    tur_cloneable_cont *cont = (tur_cloneable_cont *)(intptr_t)cont_int;\n");
    buf_puts(out, "    if (!cont) return 0;\n");
    buf_puts(out, "    tur_cloneable_cont *copy = malloc(sizeof(tur_cloneable_cont));\n");
    buf_puts(out, "    if (!copy) abort();\n");
    buf_puts(out, "    copy->cont_fn   = cont->cont_fn;\n");
    buf_puts(out, "    copy->clone_env = cont->clone_env;\n");
    buf_puts(out, "    copy->drop_env  = cont->drop_env;\n");
    buf_puts(out, "    copy->env = cont->clone_env ? cont->clone_env(cont->env) : cont->env;\n");
    buf_puts(out, "    return (int64_t)(intptr_t)copy;\n");
    buf_puts(out, "}\n\n");
    /* MS0: tur_continuation_snapshot — alias for clone; used by ^multishot resume */
    buf_puts(out, "#define tur_continuation_snapshot tur_cloneable_cont_clone\n\n");
    /* User-facing: drop — fire drop_env, then free struct */
    buf_puts(out, "static void tur_cloneable_cont_drop(int64_t cont_int) {\n");
    buf_puts(out, "    tur_cloneable_cont *cont = (tur_cloneable_cont *)(intptr_t)cont_int;\n");
    buf_puts(out, "    if (!cont) return;\n");
    buf_puts(out, "    if (cont->env && cont->drop_env) cont->drop_env(cont->env);\n");
    buf_puts(out, "    free(cont);\n");
    buf_puts(out, "}\n\n");
    /* CPS-CL4: Reset context for setjmp/longjmp-based cloneable continuations.
     * Each cloneable-reset pushes a context on this thread-local stack;
     * cloneable-shift stores k_fn's return value and longjmps back. */
    buf_puts(out, "/* CPS-CL4: cloneable-reset context */\n");
    buf_puts(out, "typedef struct tur_cloneable_reset_ctx {\n");
    buf_puts(out, "    jmp_buf jmp;\n");
    buf_puts(out, "    int64_t result;  /* k_fn return value, set by shift before longjmp */\n");
    buf_puts(out, "    struct tur_cloneable_reset_ctx *prev; /* for nested resets */\n");
    buf_puts(out, "} tur_cloneable_reset_ctx;\n\n");
    buf_puts(out, "static __thread tur_cloneable_reset_ctx *tur_current_reset_ctx = NULL;\n\n");

    } /* end if (cps_expr_contains_cloneable_shift(program)) */

    /* Phase 19: Effect handler chain */
    buf_puts(out, "/* Phase 19: Algebraic effect handler chain */\n");
    /* Phase P19-5: TurContK — a lightweight per-invocation continuation token.
     * Each tur_effect_perform call allocates one on the stack and passes its
     * address as `k` to the handler function.  `consumed` is set by (resume k v)
     * so that (cont? k) can verify freshness at runtime. */
    buf_puts(out, "typedef struct { bool consumed; void *origin_fiber; } TurContK;\n\n");
    /* Phase 19D: Effect-capture continuation context */
    buf_puts(out, "/* Phase 19D: Effect-capture continuation context */\n");
    buf_puts(out, "typedef struct TurEffectCaptureCtx TurEffectCaptureCtx;\n");
    buf_puts(out, "struct TurEffectCaptureCtx {\n");
    buf_puts(out, "    bool has_pending_effect;\n");
    buf_puts(out, "    const char *eff_name;\n");
    buf_puts(out, "    int64_t eff_args[8];  /* v1: max 8 args */\n");
    buf_puts(out, "    int eff_n_args;\n");
    buf_puts(out, "    int64_t (*dispatch)(void *ctx, int64_t k, int64_t v);\n");
    buf_puts(out, "    void *body_env;  /* heap-allocated env for body captures */\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "typedef struct EffectHandlerCase EffectHandlerCase;\n");
    buf_puts(out, "struct EffectHandlerCase {\n");
    buf_puts(out, "    const char *effect_name;\n");
    buf_puts(out, "    int64_t (*handler_fn)(int64_t *args, int n_args, int64_t k, void *env);\n");
    buf_puts(out, "    void *env;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "typedef struct EffectHandlerFrame EffectHandlerFrame;\n");
    buf_puts(out, "struct EffectHandlerFrame {\n");
    buf_puts(out, "    struct EffectHandlerFrame *parent;\n");
    buf_puts(out, "    int n_cases;\n");
    buf_puts(out, "    EffectHandlerCase cases[8];\n");
    buf_puts(out, "};\n\n");
        /* Phase T21-A/B / P19-8: FiberBlock — cooperative fiber runtime via ucontext_t.
     * tur_current_fiber is thread-local; set/restored by tur_fiber_block_resume.
     * tur_effect_perform checks tur_current_fiber->effect_handler_chain for fiber-local
     * effect handler chains (Phase T21-B / P19-8). */
    buf_puts(out, "/* Phase T21: FiberBlock */\n");
    buf_puts(out, "#ifdef __clang__\n");
    buf_puts(out, "#pragma clang diagnostic push\n");
    buf_puts(out, "#pragma clang diagnostic ignored \"-Wdeprecated-declarations\"\n");
    buf_puts(out, "#endif\n");
    buf_puts(out, "typedef struct FiberBlock FiberBlock;\n");
    buf_puts(out, "struct FiberBlock {\n");
    buf_puts(out, "    ucontext_t ctx;\n");
    buf_puts(out, "    ucontext_t caller_ctx;\n");
    buf_puts(out, "    char *stack;\n");
    buf_puts(out, "    size_t stack_size;\n");
    buf_puts(out, "    int done;\n");
    buf_puts(out, "    int parked; /* Phase T21: scheduler park/unpark */\n");
    buf_puts(out, "    int64_t result;\n");
    buf_puts(out, "    int64_t arg;\n");
    buf_puts(out, "    void *effect_handler_chain; /* Phase P19-8: per-fiber effect handler chain */\n");
    buf_puts(out, "    bool migration_safe; /* SCH-004: true if effect handlers are safe for cross-thread migration */\n");
    buf_puts(out, "    void (*entry_fn)(void);\n");
    buf_puts(out, "    void *fiber_local; /* Phase T21: fiber-local storage */\n");
    /* Phase T22: Structured concurrency */
    buf_puts(out, "    void *task_group; /* Parent TaskGroup for cancellation */\n");
    buf_puts(out, "    bool cancelled; /* Set when parent TaskGroup is cancelled */\n");
    /* Phase TG-004-1 PR: Per-fiber panic handling for auto-cancel propagation */
    buf_puts(out, "    jmp_buf panic_jmpbuf; /* Per-fiber panic recovery buffer */\n");
    buf_puts(out, "    bool panic_jmpbuf_valid; /* Whether this fiber's panic handler is active */\n");
    buf_puts(out, "    void *eff_ctx;  /* Phase 19D: NULL for regular fibers, TurEffectCaptureCtx* for effect-capture fibers */\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static __thread FiberBlock *tur_current_fiber = NULL;\n");
    /* Phase R2: tur_panic_with body — placed here so FiberBlock and
     * tur_current_fiber are in scope for the per-fiber panic check. */
    buf_puts(out, "static void tur_panic_with(int type_tag, void *payload, const char *file, int line) {\n");
    buf_puts(out, "    if (tur_panic_in_progress) {\n");
    buf_puts(out, "        fprintf(stderr, \"double panic: aborting\\n\");\n");
    buf_puts(out, "        free(payload);\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_panic_in_progress = 1;\n");
    /* Phase TG-004-2 PR: Check global handler first (try/catch has priority), then fiber */
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        global_panic_payload = panic_payload_new(type_tag, payload, file, line);\n");
    buf_puts(out, "        longjmp(global_panic_jmpbuf, 1);\n");
    buf_puts(out, "    } else if (tur_current_fiber && tur_current_fiber->panic_jmpbuf_valid) {\n");
    buf_puts(out, "        /* Use per-fiber panic buffer - set up global payload for cleanup */\n");
    buf_puts(out, "        global_panic_payload = panic_payload_new(type_tag, payload, file, line);\n");
    buf_puts(out, "        longjmp(tur_current_fiber->panic_jmpbuf, 1);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    fprintf(stderr, \"panic at %s:%d\\n\", file ? file : \"(unknown)\", line);\n");
    buf_puts(out, "    free(payload);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");
    /* Phase T22: Cooperative cancellation flag */
    buf_puts(out, "static __thread bool tur_fiber_cancelled_flag = false;\n\n");
    /* Phase T22: TaskGroup notification forward declaration */
    buf_puts(out, "static void tur_task_group_notify_done(void *task_group);\n");
    /* Forward-declare tur_fiber_set_cancelled before tur_fiber_shim uses it */
    buf_puts(out, "static void tur_fiber_set_cancelled(bool c);\n\n");
    /* TC0: per-thread cooperative cancellation state for POSIX threads */
    buf_puts(out, "/* TC0: per-thread cancel state for cooperative POSIX-thread cancellation */\n");
    buf_puts(out, "typedef struct {\n");
    buf_puts(out, "    volatile int    cancel_requested;  /* set by canceller via tur_thread_cancel */\n");
    buf_puts(out, "    pthread_mutex_t cancel_mutex;\n");
    buf_puts(out, "    pthread_cond_t  cancel_cond;\n");
    buf_puts(out, "} TurThreadState;\n\n");
    buf_puts(out, "typedef struct {\n");
    buf_puts(out, "    pthread_t        tid;\n");
    buf_puts(out, "    TurThreadState  *state;  /* heap-allocated; owned by this handle */\n");
    buf_puts(out, "} TurThreadHandle;\n\n");
    buf_puts(out, "typedef struct {\n");
    buf_puts(out, "    void           *(*user_fn)(void *);\n");
    buf_puts(out, "    void           *user_arg;\n");
    buf_puts(out, "    TurThreadState *state;\n");
    buf_puts(out, "} TurThreadSpawnArg;\n\n");
    buf_puts(out, "/* TC0: thread-local pointer to this thread's cancel state (NULL on main thread) */\n");
    buf_puts(out, "static __thread TurThreadState *tur_current_thread_state = NULL;\n");
    buf_puts(out, "/* TC0: thread-local setjmp buffer for with-cancel-guard (0 = not active) */\n");
    buf_puts(out, "static __thread jmp_buf tur_cancel_jmpbuf;\n");
    buf_puts(out, "static __thread int tur_cancel_jmpbuf_valid = 0;\n\n");
    buf_puts(out, "static void *tur_thread_trampoline(void *raw) {\n");
    buf_puts(out, "    TurThreadSpawnArg *a = (TurThreadSpawnArg *)raw;\n");
    buf_puts(out, "    tur_current_thread_state = a->state;\n");
    buf_puts(out, "    void *(*fn)(void *) = a->user_fn;\n");
    buf_puts(out, "    void *user_arg = a->user_arg;\n");
    buf_puts(out, "    free(a);\n");
    buf_puts(out, "    return fn(user_arg);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int tur_thread_cancel_requested(void) {\n");
    buf_puts(out, "    TurThreadState *s = tur_current_thread_state;\n");
    buf_puts(out, "    return s ? __atomic_load_n(&s->cancel_requested, __ATOMIC_ACQUIRE) : 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "/* TC0: cancel action -- longjmp into cancel guard if active, else exit thread */\n");
    buf_puts(out, "static void tur_thread_do_cancel(void) {\n");
    buf_puts(out, "    if (tur_cancel_jmpbuf_valid) {\n");
    buf_puts(out, "        tur_cancel_jmpbuf_valid = 0;\n");
    buf_puts(out, "        longjmp(tur_cancel_jmpbuf, 1);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* No cancel guard -- exit the thread cleanly without panicking. */\n");
    buf_puts(out, "    pthread_exit(NULL);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_shim(uint32_t hi, uint32_t lo) {\n");
    buf_puts(out, "    FiberBlock *f = (FiberBlock *)(((uintptr_t)hi << 32) | (uintptr_t)(uint32_t)lo);\n");
    /* Phase TG-004-3 PR: Per-fiber panic handling with auto-cancel on panic */
    buf_puts(out, "    void *task_group = f->task_group;\n");
    buf_puts(out, "    if (task_group) {\n");
    buf_puts(out, "        /* Save previous global panic handler state */\n");
    buf_puts(out, "        int prev_global_valid = global_panic_jmpbuf_valid;\n");
    buf_puts(out, "        jmp_buf prev_global_buf;\n");
    buf_puts(out, "        if (prev_global_valid) memcpy(&prev_global_buf, &global_panic_jmpbuf, sizeof(jmp_buf));\n");
    buf_puts(out, "        /* Clear global to prevent interference */\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        /* Set up per-fiber panic handler */\n");
    buf_puts(out, "        if (setjmp(f->panic_jmpbuf) == 0) {\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "            tur_current_fiber = f;\n");
    buf_puts(out, "            f->entry_fn();\n");
    buf_puts(out, "            tur_current_fiber = NULL;\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "            if (global_panic_payload) {\n");
    buf_puts(out, "                panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "                global_panic_payload = NULL;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            /* Restore previous global panic handler */\n");
    buf_puts(out, "            global_panic_jmpbuf_valid = prev_global_valid;\n");
    buf_puts(out, "            if (prev_global_valid) memcpy(&global_panic_jmpbuf, &prev_global_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            /* Panic caught - auto-cancel task group (TG-004-3) */\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "            tur_current_fiber = NULL;\n");
    buf_puts(out, "            if (global_panic_payload) {\n");
    buf_puts(out, "                typedef struct TaskGroupBlock { bool cancelled; bool done; int64_t cancel_reason; pthread_mutex_t lock; pthread_cond_t done_cond; } TaskGroupBlock;\n");
    buf_puts(out, "                TaskGroupBlock *g = (TaskGroupBlock *)task_group;\n");
    buf_puts(out, "                pthread_mutex_lock(&g->lock);\n");
    buf_puts(out, "                g->cancelled = true;\n");
    buf_puts(out, "                g->done = true;\n");
    buf_puts(out, "                g->cancel_reason = 1; /* panic reason (TG-004-2) */\n");
    buf_puts(out, "                pthread_cond_broadcast(&g->done_cond);\n");
    buf_puts(out, "                pthread_mutex_unlock(&g->lock);\n");
    buf_puts(out, "                tur_fiber_set_cancelled(true);\n");
    buf_puts(out, "                panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "                global_panic_payload = NULL;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            /* Restore previous global panic handler */\n");
    buf_puts(out, "            global_panic_jmpbuf_valid = prev_global_valid;\n");
    buf_puts(out, "            if (prev_global_valid) memcpy(&global_panic_jmpbuf, &prev_global_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        /* No task group, just run the function normally */\n");
    buf_puts(out, "        f->entry_fn();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    f->done = 1;\n");
    /* Phase T22: Notify task group on completion (TG-002: only if task_group is set) */
    buf_puts(out, "    if (task_group) tur_task_group_notify_done(task_group);\n");
    buf_puts(out, "    swapcontext(&f->ctx, &f->caller_ctx);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");
    /* Declare global_effect_handler_chain before tur_fiber_block_new uses it */
    buf_puts(out, "static __thread EffectHandlerFrame *global_effect_handler_chain = NULL;\n\n");
    buf_puts(out, "static FiberBlock *tur_fiber_block_new(void (*fn)(void), size_t stack_size) {\n");
    buf_puts(out, "    if (!stack_size) stack_size = 1024 * 1024;\n");
    buf_puts(out, "    FiberBlock *f = (FiberBlock *)calloc(1, sizeof(FiberBlock));\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"fiber: oom\\n\"); abort(); }\n");
    buf_puts(out, "    f->stack = (char *)malloc(stack_size);\n");
    buf_puts(out, "    if (!f->stack) { free(f); abort(); }\n");
    buf_puts(out, "    f->stack_size = stack_size; f->entry_fn = fn; f->done = 0;\n");
    /* SCH-004: Initialize migration_safe flag - fibers are migration-safe by default */
    buf_puts(out, "    f->migration_safe = true;\n");
    /* SCH-004: Copy global effect handler chain to fiber for migration safety */
    buf_puts(out, "    f->effect_handler_chain = (void *)global_effect_handler_chain;\n");
    buf_puts(out, "    getcontext(&f->ctx);\n");
    buf_puts(out, "    f->ctx.uc_stack.ss_sp = f->stack;\n");
    buf_puts(out, "    f->ctx.uc_stack.ss_size = stack_size;\n");
    buf_puts(out, "    f->ctx.uc_link = NULL;\n");
    buf_puts(out, "    uintptr_t _fp = (uintptr_t)f;\n");
    buf_puts(out, "    uint32_t _hi = (uint32_t)(_fp >> 32);\n");
    buf_puts(out, "    uint32_t _lo = (uint32_t)(_fp & 0xFFFFFFFFU);\n");
    buf_puts(out, "    makecontext(&f->ctx, (void(*)(void))tur_fiber_shim, 2, _hi, _lo);\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_fiber_block_resume(FiberBlock *f, int64_t arg) {\n");
    buf_puts(out, "    if (!f || f->done) return f ? f->result : 0;\n");
    /* Phase T22: Check if fiber or its task group was cancelled before resuming */
    buf_puts(out, "    if (f->cancelled) { f->done = 1; return 0; }\n");
    buf_puts(out, "    if (f->task_group) {\n");
    buf_puts(out, "        typedef struct TaskGroupBlock { bool cancelled; } TaskGroupBlock;\n");
    buf_puts(out, "        if (((TaskGroupBlock *)f->task_group)->cancelled) { f->cancelled = 1; f->done = 1; return 0; }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    FiberBlock *_prev = tur_current_fiber;\n");
    buf_puts(out, "    tur_current_fiber = f;\n");
    buf_puts(out, "    f->arg = arg;\n");
    buf_puts(out, "    swapcontext(&f->caller_ctx, &f->ctx);\n");
    buf_puts(out, "    tur_current_fiber = _prev;\n");
    buf_puts(out, "    return f->result;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_block_yield(int64_t value) {\n");
    buf_puts(out, "    FiberBlock *f = tur_current_fiber;\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"fiber-yield: not in fiber\\n\"); abort(); }\n");
    buf_puts(out, "    f->result = value;\n");
    buf_puts(out, "    swapcontext(&f->ctx, &f->caller_ctx);\n");
    buf_puts(out, "}\n\n");
    /* Phase 19D: Effect-capture continuation helpers */
    buf_puts(out, "/* Phase 19D: Effect-capture continuation helpers */\n");
    buf_puts(out, "static int64_t tur_effect_cont_resume(int64_t k_as_int64, int64_t v) {\n");
    buf_puts(out, "    FiberBlock *fiber = (FiberBlock *)(intptr_t)k_as_int64;\n");
    buf_puts(out, "    TurEffectCaptureCtx *ctx = (TurEffectCaptureCtx *)fiber->eff_ctx;\n");
    buf_puts(out, "    if (!ctx) { fprintf(stderr, \"continuation error: not a capturable continuation\\n\"); abort(); }\n");
    buf_puts(out, "    return ctx->dispatch(ctx, k_as_int64, v);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static bool tur_effect_cont_valid(int64_t k_as_int64) {\n");
    buf_puts(out, "    FiberBlock *fiber = (FiberBlock *)(intptr_t)k_as_int64;\n");
    buf_puts(out, "    return !fiber->done;\n");
    buf_puts(out, "}\n\n");
    /* Phase T21: Fiber-local storage */
    buf_puts(out, "typedef struct FiberLocalEntry FiberLocalEntry;\n");
    buf_puts(out, "struct FiberLocalEntry {\n");
    buf_puts(out, "    int64_t key;\n");
    buf_puts(out, "    int64_t value;\n");
    buf_puts(out, "    FiberLocalEntry *next;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static void tur_fiber_local_free(FiberBlock *f) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { FiberLocalEntry *n = e->next; free(e); e = n; }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_fiber_local_get(FiberBlock *f, int64_t key) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { if (e->key == key) return e->value; e = e->next; }\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_local_set(FiberBlock *f, int64_t key, int64_t value) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { if (e->key == key) { e->value = value; return; } e = e->next; }\n");
    buf_puts(out, "    FiberLocalEntry *n = (FiberLocalEntry *)malloc(sizeof(FiberLocalEntry));\n");
    buf_puts(out, "    if (!n) { fprintf(stderr, \"fiber-local: oom\\n\"); abort(); }\n");
    buf_puts(out, "    n->key = key; n->value = value;\n");
    buf_puts(out, "    n->next = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    f->fiber_local = (void *)n;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_block_free(FiberBlock *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    tur_fiber_local_free(f);\n");
    buf_puts(out, "    free(f->stack); free(f);\n");
    buf_puts(out, "}\n\n");
    /* Phase T22: Fiber cancellation */
    buf_puts(out, "static bool tur_fiber_cancelled(void) {\n");
    buf_puts(out, "    return tur_fiber_cancelled_flag;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_set_cancelled(bool c) {\n");
    buf_puts(out, "    tur_fiber_cancelled_flag = c;\n");
    buf_puts(out, "}\n\n");
    /* Phase T22: TaskGroup notification on fiber completion */
    buf_puts(out, "static void tur_task_group_notify_done(void *task_group) {\n");
    buf_puts(out, "    if (!task_group) return;\n");
    buf_puts(out, "    typedef struct TaskGroupBlock {\n");
    buf_puts(out, "        pthread_mutex_t lock;\n");
    buf_puts(out, "        pthread_cond_t done_cond;\n");
    buf_puts(out, "        int64_t task_count;\n");
    buf_puts(out, "        int64_t completed_count;\n");
    buf_puts(out, "        bool cancelled;\n");
    buf_puts(out, "        bool done;\n");
    buf_puts(out, "        pthread_t owner_thread;\n");
    buf_puts(out, "    } TaskGroupBlock;\n");
    buf_puts(out, "    TaskGroupBlock *g = (TaskGroupBlock *)task_group;\n");
    buf_puts(out, "    pthread_mutex_lock(&g->lock);\n");
    buf_puts(out, "    g->completed_count++;\n");
    buf_puts(out, "    if (g->completed_count >= g->task_count) {\n");
    buf_puts(out, "        g->done = true;\n");
    buf_puts(out, "        pthread_cond_broadcast(&g->done_cond);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    pthread_mutex_unlock(&g->lock);\n");
    buf_puts(out, "}\n\n");
    /* Phase T24: Forward declarations for timer wheel and IO waiters
     * (defined after scheduler, but referenced in scheduler run loops) */
    /* Phase T24: Timer wheel structs + globals (defined early so scheduler
     * run loops and helper functions can reference them) */
    buf_puts(out, "static int64_t tur_monotonic_ns(void) {\n");
    buf_puts(out, "    struct timespec ts;\n");
    buf_puts(out, "    clock_gettime(CLOCK_MONOTONIC, &ts);\n");
    buf_puts(out, "    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "typedef struct TurTimerEntry {\n");
    buf_puts(out, "    int64_t deadline_ns;\n");
    buf_puts(out, "    void (*callback)(void *);\n");
    buf_puts(out, "    void *arg;\n");
    buf_puts(out, "    int64_t id;\n");
    buf_puts(out, "    bool cancelled;\n");
    buf_puts(out, "} TurTimerEntry;\n\n");

    buf_puts(out, "typedef struct TurTimerWheel {\n");
    buf_puts(out, "    TurTimerEntry **heap;\n");
    buf_puts(out, "    int64_t len;\n");
    buf_puts(out, "    int64_t cap;\n");
    buf_puts(out, "    int64_t next_id;\n");
    buf_puts(out, "} TurTimerWheel;\n\n");

    buf_puts(out, "static TurTimerWheel *tur_global_timers = NULL;\n\n");

    buf_puts(out, "#define TUR_IO_READ  1\n");
    buf_puts(out, "#define TUR_IO_WRITE 2\n\n");

    buf_puts(out, "typedef struct TurIOWaiter {\n");
    buf_puts(out, "    int fd;\n");
    buf_puts(out, "    int events;\n");
    buf_puts(out, "    FiberBlock *fiber;\n");
    buf_puts(out, "    struct TurIOWaiter *next;\n");
    buf_puts(out, "} TurIOWaiter;\n\n");

    buf_puts(out, "static TurIOWaiter *tur_io_waiters = NULL;\n\n");

    /* Forward-declare timer/IO functions used in scheduler run loops */
    buf_puts(out, "static void tur_timer_wheel_tick(TurTimerWheel *w);\n");
    buf_puts(out, "static int64_t tur_timer_wheel_next_deadline_ns(TurTimerWheel *w);\n");
    buf_puts(out, "static int64_t tur_timer_wheel_insert(TurTimerWheel *w, int64_t deadline_ns, void (*cb)(void *), void *arg);\n");
    buf_puts(out, "static void tur_timer_wheel_cancel(TurTimerWheel *w, int64_t id);\n");
    buf_puts(out, "static TurTimerWheel *tur_timer_wheel_new(void);\n");
    buf_puts(out, "static void tur_io_register(int fd, int events, FiberBlock *fiber);\n");
    buf_puts(out, "static void tur_io_unregister(int fd);\n");
    buf_puts(out, "static void tur_io_poll(int64_t timeout_us);\n");
    buf_puts(out, "static void tur_scheduler_timeout(int64_t ms, void (*callback)(void *arg), void *arg);\n");
    buf_puts(out, "static void tur_tick_timers(void);\n");
    buf_puts(out, "static void tur_poll_io(int64_t timeout_us);\n");
    buf_puts(out, "static bool tur_has_pending_timers(void);\n");
    buf_puts(out, "static bool tur_has_pending_io(void);\n");
    buf_puts(out, "static int64_t tur_next_timer_wait_us(void);\n\n");

    /* Phase T21: Cooperative Scheduler for fibers */
    buf_puts(out, "typedef struct TurScheduler TurScheduler;\n");
    buf_puts(out, "struct TurScheduler {\n");
    buf_puts(out, "    FiberBlock **run_queue;\n");
    buf_puts(out, "    int64_t run_queue_cap;\n");
    buf_puts(out, "    int64_t run_queue_len;\n");
    buf_puts(out, "    int64_t run_queue_head;\n");
    buf_puts(out, "    int64_t run_queue_tail;\n");
    buf_puts(out, "    FiberBlock *current_fiber;\n");
    buf_puts(out, "    bool running;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler = NULL;\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler_new(void) {\n");
    buf_puts(out, "    TurScheduler *s = (TurScheduler *)calloc(1, sizeof(TurScheduler));\n");
    buf_puts(out, "    if (!s) { fprintf(stderr, \"scheduler: oom\\n\"); abort(); }\n");
    buf_puts(out, "    s->run_queue_cap = 64;\n");
    buf_puts(out, "    s->run_queue = (FiberBlock **)malloc(sizeof(FiberBlock *) * (size_t)s->run_queue_cap);\n");
    buf_puts(out, "    if (!s->run_queue) { free(s); fprintf(stderr, \"scheduler: queue oom\\n\"); abort(); }\n");
    buf_puts(out, "    s->run_queue_len = 0;\n");
    buf_puts(out, "    s->run_queue_head = 0;\n");
    buf_puts(out, "    s->run_queue_tail = 0;\n");
    buf_puts(out, "    s->current_fiber = NULL;\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "    return s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler_current(void) {\n");
    buf_puts(out, "    return tur_scheduler;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_enqueue(TurScheduler *s, FiberBlock *f) {\n");
    buf_puts(out, "    if (s->run_queue_len >= s->run_queue_cap) {\n");
    buf_puts(out, "        int64_t new_cap = s->run_queue_cap * 2;\n");
    buf_puts(out, "        FiberBlock **new_q = (FiberBlock **)malloc(sizeof(FiberBlock *) * (size_t)new_cap);\n");
    buf_puts(out, "        if (!new_q) { fprintf(stderr, \"scheduler: grow oom\\n\"); abort(); }\n");
    buf_puts(out, "        for (int64_t i = 0; i < s->run_queue_len; i++)\n");
    buf_puts(out, "            new_q[i] = s->run_queue[(s->run_queue_head + i) % s->run_queue_cap];\n");
    buf_puts(out, "        free(s->run_queue);\n");
    buf_puts(out, "        s->run_queue = new_q;\n");
    buf_puts(out, "        s->run_queue_cap = new_cap;\n");
    buf_puts(out, "        s->run_queue_head = 0;\n");
    buf_puts(out, "        s->run_queue_tail = s->run_queue_len;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->run_queue[s->run_queue_tail] = f;\n");
    buf_puts(out, "    s->run_queue_tail = (s->run_queue_tail + 1) % s->run_queue_cap;\n");
    buf_puts(out, "    s->run_queue_len++;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static FiberBlock *tur_scheduler_dequeue(TurScheduler *s) {\n");
    buf_puts(out, "    if (s->run_queue_len == 0) return NULL;\n");
    buf_puts(out, "    FiberBlock *f = s->run_queue[s->run_queue_head];\n");
    buf_puts(out, "    s->run_queue_head = (s->run_queue_head + 1) % s->run_queue_cap;\n");
    buf_puts(out, "    s->run_queue_len--;\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_spawn(TurScheduler *s, FiberBlock *f) {\n");
    buf_puts(out, "    tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_run(TurScheduler *s) {\n");
    buf_puts(out, "    s->running = true;\n");
    buf_puts(out, "    while (s->running) {\n");
    buf_puts(out, "        /* Phase T24: tick timers and poll IO before dequeuing */\n");
    buf_puts(out, "        tur_tick_timers();\n");
    buf_puts(out, "        tur_poll_io(0);\n");
    buf_puts(out, "        FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "        if (!f) {\n");
    buf_puts(out, "            if (!tur_has_pending_timers() && !tur_has_pending_io()) break;\n");
    buf_puts(out, "            int64_t sleep_us = tur_next_timer_wait_us();\n");
    buf_puts(out, "            if (sleep_us < 0) sleep_us = 1000;\n");
    buf_puts(out, "            if (tur_has_pending_io()) tur_poll_io(sleep_us);\n");
    buf_puts(out, "            else { struct timespec ts = {0, sleep_us * 1000}; nanosleep(&ts, NULL); }\n");
    buf_puts(out, "            continue;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        s->current_fiber = f;\n");
    buf_puts(out, "        tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "        s->current_fiber = NULL;\n");
    buf_puts(out, "        if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_run_to_completion(TurScheduler *s) {\n");
    buf_puts(out, "    s->running = true;\n");
    buf_puts(out, "    while (s->running) {\n");
    buf_puts(out, "        tur_tick_timers();\n");
    buf_puts(out, "        tur_poll_io(0);\n");
    buf_puts(out, "        FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "        if (f) {\n");
    buf_puts(out, "            s->current_fiber = f;\n");
    buf_puts(out, "            tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "            s->current_fiber = NULL;\n");
    buf_puts(out, "            if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            if (!tur_has_pending_timers() && !tur_has_pending_io()) break;\n");
    buf_puts(out, "            int64_t sleep_us = tur_next_timer_wait_us();\n");
    buf_puts(out, "            if (sleep_us < 0) sleep_us = 1000;\n");
    buf_puts(out, "            if (tur_has_pending_io()) tur_poll_io(sleep_us);\n");
    buf_puts(out, "            else { struct timespec ts = {0, sleep_us * 1000}; nanosleep(&ts, NULL); }\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "}\n\n");
    /* Phase T21: Scheduler yield/park/unpark */
    buf_puts(out, "static void tur_scheduler_yield(void) {\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_park(void) {\n");
    buf_puts(out, "    if (tur_current_fiber) tur_current_fiber->parked = 1;\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_unpark(FiberBlock *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    f->parked = 0;\n");
    buf_puts(out, "    if (tur_scheduler) tur_scheduler_enqueue(tur_scheduler, f);\n");
    buf_puts(out, "}\n\n");

    /* Phase T24: Timer wheel function implementations
     * (structs, globals, and tur_monotonic_ns defined earlier before scheduler) */
    buf_puts(out, "static TurTimerWheel *tur_timer_wheel_new(void) {\n");
    buf_puts(out, "    TurTimerWheel *w = (TurTimerWheel *)calloc(1, sizeof(TurTimerWheel));\n");
    buf_puts(out, "    if (!w) { fprintf(stderr, \"timer wheel: oom\\n\"); abort(); }\n");
    buf_puts(out, "    w->cap = 64;\n");
    buf_puts(out, "    w->heap = (TurTimerEntry **)malloc(sizeof(TurTimerEntry *) * (size_t)w->cap);\n");
    buf_puts(out, "    if (!w->heap) { free(w); fprintf(stderr, \"timer wheel: heap oom\\n\"); abort(); }\n");
    buf_puts(out, "    w->len = 0;\n");
    buf_puts(out, "    w->next_id = 1;\n");
    buf_puts(out, "    return w;\n");
    buf_puts(out, "}\n\n");

    /* Min-heap helpers: sift up and sift down by deadline */
    buf_puts(out, "static void tur_timer_heap_sift_up(TurTimerWheel *w, int64_t idx) {\n");
    buf_puts(out, "    while (idx > 0) {\n");
    buf_puts(out, "        int64_t parent = (idx - 1) / 2;\n");
    buf_puts(out, "        if (w->heap[parent]->deadline_ns <= w->heap[idx]->deadline_ns) break;\n");
    buf_puts(out, "        TurTimerEntry *tmp = w->heap[parent];\n");
    buf_puts(out, "        w->heap[parent] = w->heap[idx];\n");
    buf_puts(out, "        w->heap[idx] = tmp;\n");
    buf_puts(out, "        idx = parent;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_timer_heap_sift_down(TurTimerWheel *w, int64_t idx) {\n");
    buf_puts(out, "    while (1) {\n");
    buf_puts(out, "        int64_t smallest = idx;\n");
    buf_puts(out, "        int64_t left = 2 * idx + 1;\n");
    buf_puts(out, "        int64_t right = 2 * idx + 2;\n");
    buf_puts(out, "        if (left < w->len && w->heap[left]->deadline_ns < w->heap[smallest]->deadline_ns)\n");
    buf_puts(out, "            smallest = left;\n");
    buf_puts(out, "        if (right < w->len && w->heap[right]->deadline_ns < w->heap[smallest]->deadline_ns)\n");
    buf_puts(out, "            smallest = right;\n");
    buf_puts(out, "        if (smallest == idx) break;\n");
    buf_puts(out, "        TurTimerEntry *tmp = w->heap[smallest];\n");
    buf_puts(out, "        w->heap[smallest] = w->heap[idx];\n");
    buf_puts(out, "        w->heap[idx] = tmp;\n");
    buf_puts(out, "        idx = smallest;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static int64_t tur_timer_wheel_insert(TurTimerWheel *w, int64_t deadline_ns,\n");
    buf_puts(out, "                                       void (*cb)(void *), void *arg) {\n");
    buf_puts(out, "    if (w->len >= w->cap) {\n");
    buf_puts(out, "        int64_t new_cap = w->cap * 2;\n");
    buf_puts(out, "        TurTimerEntry **nh = (TurTimerEntry **)malloc(sizeof(TurTimerEntry *) * (size_t)new_cap);\n");
    buf_puts(out, "        if (!nh) { fprintf(stderr, \"timer wheel: grow oom\\n\"); abort(); }\n");
    buf_puts(out, "        for (int64_t i = 0; i < w->len; i++) nh[i] = w->heap[i];\n");
    buf_puts(out, "        free(w->heap);\n");
    buf_puts(out, "        w->heap = nh;\n");
    buf_puts(out, "        w->cap = new_cap;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    TurTimerEntry *e = (TurTimerEntry *)malloc(sizeof(TurTimerEntry));\n");
    buf_puts(out, "    if (!e) { fprintf(stderr, \"timer entry: oom\\n\"); abort(); }\n");
    buf_puts(out, "    e->deadline_ns = deadline_ns;\n");
    buf_puts(out, "    e->callback = cb;\n");
    buf_puts(out, "    e->arg = arg;\n");
    buf_puts(out, "    e->id = w->next_id++;\n");
    buf_puts(out, "    e->cancelled = false;\n");
    buf_puts(out, "    w->heap[w->len] = e;\n");
    buf_puts(out, "    tur_timer_heap_sift_up(w, w->len);\n");
    buf_puts(out, "    w->len++;\n");
    buf_puts(out, "    return e->id;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_timer_wheel_cancel(TurTimerWheel *w, int64_t id) {\n");
    buf_puts(out, "    for (int64_t i = 0; i < w->len; i++) {\n");
    buf_puts(out, "        if (w->heap[i]->id == id) { w->heap[i]->cancelled = true; return; }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_timer_wheel_tick(TurTimerWheel *w) {\n");
    buf_puts(out, "    int64_t now = tur_monotonic_ns();\n");
    buf_puts(out, "    while (w->len > 0 && w->heap[0]->deadline_ns <= now) {\n");
    buf_puts(out, "        TurTimerEntry *e = w->heap[0];\n");
    buf_puts(out, "        w->len--;\n");
    buf_puts(out, "        if (w->len > 0) {\n");
    buf_puts(out, "            w->heap[0] = w->heap[w->len];\n");
    buf_puts(out, "            tur_timer_heap_sift_down(w, 0);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        if (!e->cancelled) e->callback(e->arg);\n");
    buf_puts(out, "        free(e);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static int64_t tur_timer_wheel_next_deadline_ns(TurTimerWheel *w) {\n");
    buf_puts(out, "    while (w->len > 0 && w->heap[0]->cancelled) {\n");
    buf_puts(out, "        TurTimerEntry *e = w->heap[0];\n");
    buf_puts(out, "        w->len--;\n");
    buf_puts(out, "        if (w->len > 0) {\n");
    buf_puts(out, "            w->heap[0] = w->heap[w->len];\n");
    buf_puts(out, "            tur_timer_heap_sift_down(w, 0);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        free(e);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (w->len == 0) return -1;\n");
    buf_puts(out, "    return w->heap[0]->deadline_ns;\n");
    buf_puts(out, "}\n\n");

    /* tur_scheduler_timeout: same API, now uses timer wheel instead of thread-per-timeout */
    buf_puts(out, "static void tur_scheduler_timeout(int64_t ms, void (*callback)(void *arg), void *arg) {\n");
    buf_puts(out, "    if (!tur_global_timers) tur_global_timers = tur_timer_wheel_new();\n");
    buf_puts(out, "    int64_t deadline = tur_monotonic_ns() + ms * 1000000LL;\n");
    buf_puts(out, "    tur_timer_wheel_insert(tur_global_timers, deadline, callback, arg);\n");
    buf_puts(out, "}\n\n");

    /* Phase T24: IO waiter function implementations
     * (struct, defines, and global defined earlier before scheduler) */
    buf_puts(out, "static void tur_io_register(int fd, int events, FiberBlock *fiber) {\n");
    buf_puts(out, "    TurIOWaiter *w = (TurIOWaiter *)malloc(sizeof(TurIOWaiter));\n");
    buf_puts(out, "    if (!w) { fprintf(stderr, \"io waiter: oom\\n\"); abort(); }\n");
    buf_puts(out, "    w->fd = fd;\n");
    buf_puts(out, "    w->events = events;\n");
    buf_puts(out, "    w->fiber = fiber;\n");
    buf_puts(out, "    w->next = tur_io_waiters;\n");
    buf_puts(out, "    tur_io_waiters = w;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_io_unregister(int fd) {\n");
    buf_puts(out, "    TurIOWaiter **pp = &tur_io_waiters;\n");
    buf_puts(out, "    while (*pp) {\n");
    buf_puts(out, "        if ((*pp)->fd == fd) {\n");
    buf_puts(out, "            TurIOWaiter *tmp = *pp;\n");
    buf_puts(out, "            *pp = tmp->next;\n");
    buf_puts(out, "            free(tmp);\n");
    buf_puts(out, "            return;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        pp = &(*pp)->next;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_io_poll(int64_t timeout_us) {\n");
    buf_puts(out, "    if (!tur_io_waiters) return;\n");
    buf_puts(out, "    fd_set rfds, wfds;\n");
    buf_puts(out, "    FD_ZERO(&rfds);\n");
    buf_puts(out, "    FD_ZERO(&wfds);\n");
    buf_puts(out, "    int maxfd = -1;\n");
    buf_puts(out, "    for (TurIOWaiter *w = tur_io_waiters; w; w = w->next) {\n");
    buf_puts(out, "        if (w->events & TUR_IO_READ)  FD_SET(w->fd, &rfds);\n");
    buf_puts(out, "        if (w->events & TUR_IO_WRITE) FD_SET(w->fd, &wfds);\n");
    buf_puts(out, "        if (w->fd > maxfd) maxfd = w->fd;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    struct timeval tv;\n");
    buf_puts(out, "    tv.tv_sec = (long)(timeout_us / 1000000);\n");
    buf_puts(out, "    tv.tv_usec = (long)(timeout_us % 1000000);\n");
    buf_puts(out, "    int ret = select(maxfd + 1, &rfds, &wfds, NULL, &tv);\n");
    buf_puts(out, "    if (ret <= 0) return;\n");
    buf_puts(out, "    TurIOWaiter **pp = &tur_io_waiters;\n");
    buf_puts(out, "    while (*pp) {\n");
    buf_puts(out, "        TurIOWaiter *w = *pp;\n");
    buf_puts(out, "        bool ready = false;\n");
    buf_puts(out, "        if ((w->events & TUR_IO_READ)  && FD_ISSET(w->fd, &rfds))  ready = true;\n");
    buf_puts(out, "        if ((w->events & TUR_IO_WRITE) && FD_ISSET(w->fd, &wfds)) ready = true;\n");
    buf_puts(out, "        if (ready) {\n");
    buf_puts(out, "            FiberBlock *f = w->fiber;\n");
    buf_puts(out, "            *pp = w->next;\n");
    buf_puts(out, "            free(w);\n");
    buf_puts(out, "            tur_scheduler_unpark(f);\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            pp = &w->next;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    /* Phase T24: Helper functions for scheduler integration (implementations
     * of the forward-declared functions used in the scheduler run loops) */
    buf_puts(out, "static void tur_tick_timers(void) {\n");
    buf_puts(out, "    if (tur_global_timers) tur_timer_wheel_tick(tur_global_timers);\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static void tur_poll_io(int64_t timeout_us) {\n");
    buf_puts(out, "    if (tur_io_waiters) tur_io_poll(timeout_us);\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static bool tur_has_pending_timers(void) {\n");
    buf_puts(out, "    return tur_global_timers && tur_global_timers->len > 0;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static bool tur_has_pending_io(void) {\n");
    buf_puts(out, "    return tur_io_waiters != NULL;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "static int64_t tur_next_timer_wait_us(void) {\n");
    buf_puts(out, "    if (!tur_global_timers || tur_global_timers->len == 0) return -1;\n");
    buf_puts(out, "    int64_t next = tur_timer_wheel_next_deadline_ns(tur_global_timers);\n");
    buf_puts(out, "    if (next < 0) return -1;\n");
    buf_puts(out, "    int64_t diff = (next - tur_monotonic_ns()) / 1000;\n");
    buf_puts(out, "    return diff > 0 ? diff : 0;\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "#ifdef __clang__\n");
    buf_puts(out, "#pragma clang diagnostic pop\n");
    buf_puts(out, "#endif\n\n");
    /* Phase T21: Scheduler run_one helper */
    buf_puts(out, "static void tur_scheduler_run_one(TurScheduler *s) {\n");
    buf_puts(out, "    FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "    if (!f) { return; }\n");
    buf_puts(out, "    s->current_fiber = f;\n");
    buf_puts(out, "    tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "    s->current_fiber = NULL;\n");
    buf_puts(out, "    if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "}\n\n");

    /* Phase T23: Minimal multi-threaded scheduler runtime.
     * Provides the API surface used by stdlib/scheduler_mt.tur.  This is a
     * simple shared-queue implementation (not work-stealing); sufficient to
     * run cooperative fibers across N OS threads for fixtures that just need
     * to observe distinct thread ids. */
    buf_puts(out, "/* Phase T23: Multi-threaded scheduler */\n");
    buf_puts(out, "typedef struct TurSchedulerMT {\n");
    buf_puts(out, "    pthread_t       *threads;\n");
    buf_puts(out, "    size_t           n_threads;\n");
    buf_puts(out, "    pthread_mutex_t  lock;\n");
    buf_puts(out, "    pthread_cond_t   cond;\n");
    buf_puts(out, "    FiberBlock     **queue;\n");
    buf_puts(out, "    size_t           qhead;\n");
    buf_puts(out, "    size_t           qtail;\n");
    buf_puts(out, "    size_t           qcap;\n");
    buf_puts(out, "    bool             running;\n");
    buf_puts(out, "    int              active;\n");
    buf_puts(out, "} TurSchedulerMT;\n\n");
    buf_puts(out, "static __thread TurSchedulerMT *tur_current_scheduler_mt = NULL;\n\n");
    buf_puts(out, "static void tur_scheduler_mt_enqueue_locked(TurSchedulerMT *s, FiberBlock *f) {\n");
    buf_puts(out, "    if (((s->qtail + 1) % s->qcap) == s->qhead) {\n");
    buf_puts(out, "        size_t ncap = s->qcap * 2;\n");
    buf_puts(out, "        FiberBlock **nq = (FiberBlock **)calloc(ncap, sizeof(FiberBlock *));\n");
    buf_puts(out, "        size_t k = 0;\n");
    buf_puts(out, "        for (size_t i = s->qhead; i != s->qtail; i = (i + 1) % s->qcap) nq[k++] = s->queue[i];\n");
    buf_puts(out, "        free(s->queue);\n");
    buf_puts(out, "        s->queue = nq; s->qcap = ncap; s->qhead = 0; s->qtail = k;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->queue[s->qtail] = f;\n");
    buf_puts(out, "    s->qtail = (s->qtail + 1) % s->qcap;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void *tur_scheduler_mt_worker(void *arg) {\n");
    buf_puts(out, "    TurSchedulerMT *s = (TurSchedulerMT *)arg;\n");
    buf_puts(out, "    tur_current_scheduler_mt = s;\n");
    buf_puts(out, "    for (;;) {\n");
    buf_puts(out, "        pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "        while (s->running && s->qhead == s->qtail) {\n");
    buf_puts(out, "            pthread_cond_wait(&s->cond, &s->lock);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        if (!s->running && s->qhead == s->qtail) {\n");
    buf_puts(out, "            pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "            break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        FiberBlock *f = s->queue[s->qhead];\n");
    buf_puts(out, "        s->qhead = (s->qhead + 1) % s->qcap;\n");
    buf_puts(out, "        s->active++;\n");
    buf_puts(out, "        pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "        tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "        pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "        s->active--;\n");
    buf_puts(out, "        if (!f->done && !f->parked) tur_scheduler_mt_enqueue_locked(s, f);\n");
    buf_puts(out, "        pthread_cond_broadcast(&s->cond);\n");
    buf_puts(out, "        pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static TurSchedulerMT *tur_scheduler_mt_new(size_t n_threads) {\n");
    buf_puts(out, "    if (n_threads == 0) n_threads = 1;\n");
    buf_puts(out, "    TurSchedulerMT *s = (TurSchedulerMT *)calloc(1, sizeof(TurSchedulerMT));\n");
    buf_puts(out, "    if (!s) return NULL;\n");
    buf_puts(out, "    pthread_mutex_init(&s->lock, NULL);\n");
    buf_puts(out, "    pthread_cond_init(&s->cond, NULL);\n");
    buf_puts(out, "    s->qcap = 32;\n");
    buf_puts(out, "    s->queue = (FiberBlock **)calloc(s->qcap, sizeof(FiberBlock *));\n");
    buf_puts(out, "    s->n_threads = n_threads;\n");
    buf_puts(out, "    s->threads = (pthread_t *)calloc(n_threads, sizeof(pthread_t));\n");
    buf_puts(out, "    s->running = true;\n");
    buf_puts(out, "    for (size_t i = 0; i < n_threads; i++) {\n");
    buf_puts(out, "        pthread_create(&s->threads[i], NULL, tur_scheduler_mt_worker, s);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_free(TurSchedulerMT *s) {\n");
    buf_puts(out, "    if (!s) return;\n");
    buf_puts(out, "    pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "    /* Drain: wait until queue is empty and no fiber is active */\n");
    buf_puts(out, "    while (s->qhead != s->qtail || s->active > 0) {\n");
    buf_puts(out, "        pthread_cond_wait(&s->cond, &s->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "    pthread_cond_broadcast(&s->cond);\n");
    buf_puts(out, "    pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "    for (size_t i = 0; i < s->n_threads; i++) pthread_join(s->threads[i], NULL);\n");
    buf_puts(out, "    pthread_mutex_destroy(&s->lock);\n");
    buf_puts(out, "    pthread_cond_destroy(&s->cond);\n");
    buf_puts(out, "    free(s->queue); free(s->threads); free(s);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_spawn(TurSchedulerMT *s, FiberBlock *f) {\n");
    buf_puts(out, "    if (!s || !f) return;\n");
    buf_puts(out, "    pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "    tur_scheduler_mt_enqueue_locked(s, f);\n");
    buf_puts(out, "    pthread_cond_signal(&s->cond);\n");
    buf_puts(out, "    pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_run(TurSchedulerMT *s) {\n");
    buf_puts(out, "    /* Worker threads run automatically; this is a no-op for the\n");
    buf_puts(out, "     * shared-queue impl.  Kept for API compatibility. */\n");
    buf_puts(out, "    (void)s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_scheduler_mt_thread_id(void) {\n");
    buf_puts(out, "    return (int64_t)(uintptr_t)pthread_self();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_set_current(TurSchedulerMT *s) {\n");
    buf_puts(out, "    tur_current_scheduler_mt = s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static TurSchedulerMT *tur_scheduler_mt_current(void) {\n");
    buf_puts(out, "    return tur_current_scheduler_mt;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_yield(void) {\n");
    buf_puts(out, "    /* Cooperative yield: re-enqueues current fiber via the worker loop. */\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_park(void) {\n");
    buf_puts(out, "    FiberBlock *f = tur_current_fiber;\n");
    buf_puts(out, "    if (f) f->parked = 1;\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_mt_unpark(FiberBlock *f) {\n");
    buf_puts(out, "    TurSchedulerMT *s = tur_current_scheduler_mt;\n");
    buf_puts(out, "    if (!f || !s) return;\n");
    buf_puts(out, "    f->parked = 0;\n");
    buf_puts(out, "    pthread_mutex_lock(&s->lock);\n");
    buf_puts(out, "    tur_scheduler_mt_enqueue_locked(s, f);\n");
    buf_puts(out, "    pthread_cond_signal(&s->cond);\n");
    buf_puts(out, "    pthread_mutex_unlock(&s->lock);\n");
    buf_puts(out, "}\n\n");

    /* Phase T21-F: async/await runtime - fiber-based Futures */
    buf_puts(out, "/* Phase T21-F: async/await runtime - fiber-based Futures */\n");
    buf_puts(out, "typedef enum { FUTURE_PENDING, FUTURE_FULFILLED, FUTURE_REJECTED } TurFutureStatus;\n\n");
    
    buf_puts(out, "typedef struct TurFuture TurFuture;\n");
    buf_puts(out, "struct TurFuture {\n");
    buf_puts(out, "    TurFutureStatus status;\n");
    buf_puts(out, "    int64_t value;\n");
    buf_puts(out, "    const char *error;  /* NULL if no error */\n");
    buf_puts(out, "    FiberBlock *fiber;  /* The fiber running the async task */\n");
    buf_puts(out, "    struct { void (*fn)(TurFuture *, int64_t); void *env; } on_complete;\n");
    buf_puts(out, "};\n\n");
    
    buf_puts(out, "/* Create a new pending future */\n");
    buf_puts(out, "static TurFuture *tur_future_new(void) {\n");
    buf_puts(out, "    TurFuture *f = (TurFuture *)calloc(1, sizeof(TurFuture));\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"future: oom\\n\"); abort(); }\n");
    buf_puts(out, "    f->status = FUTURE_PENDING;\n");
    buf_puts(out, "    f->fiber = NULL;\n");
    buf_puts(out, "    f->on_complete.fn = NULL;\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Fulfill a future with a value */\n");
    buf_puts(out, "static void tur_future_fulfill(TurFuture *f, int64_t value) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_PENDING) return;\n");
    buf_puts(out, "    f->status = FUTURE_FULFILLED;\n");
    buf_puts(out, "    f->value = value;\n");
    buf_puts(out, "    if (f->on_complete.fn) f->on_complete.fn(f, value);\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Reject a future with an error message */\n");
    buf_puts(out, "static void tur_future_reject(TurFuture *f, const char *error) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_PENDING) return;\n");
    buf_puts(out, "    f->status = FUTURE_REJECTED;\n");
    buf_puts(out, "    f->error = error;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Check if future is done (fulfilled or rejected) */\n");
    buf_puts(out, "static int tur_future_done(TurFuture *f) {\n");
    buf_puts(out, "    return f && (f->status == FUTURE_FULFILLED || f->status == FUTURE_REJECTED);\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Get future value (only valid if fulfilled) */\n");
    buf_puts(out, "static int64_t tur_future_get(TurFuture *f) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_FULFILLED) return 0;\n");
    buf_puts(out, "    return f->value;\n");
    buf_puts(out, "}\n\n");
    
    /* Create a future that runs fn() and fulfills it with the result */
    buf_puts(out, "static TurFuture *tur_async_fiber(int64_t (*fn)(void)) {\n");
    buf_puts(out, "    TurFuture *future = tur_future_new();\n");
    buf_puts(out, "    if (!tur_scheduler) {\n");
    buf_puts(out, "        /* AW-005: Initialize scheduler on first use */\n");
    buf_puts(out, "        tur_scheduler = tur_scheduler_new();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* AW-005: Simplified v1 - call function directly and fulfill */\n");
    buf_puts(out, "    int64_t result = fn();\n");
    buf_puts(out, "    tur_future_fulfill(future, result);\n");
    buf_puts(out, "    return future;\n");
    buf_puts(out, "}\n\n");
    
    /* Await a future using shift + scheduler. If future is done, return value directly. */
    buf_puts(out, "/* AW-004: await lowering with shift + scheduler callback */\n");
    buf_puts(out, "static int64_t tur_await_future(TurFuture *f) {\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"await: null future\\n\"); abort(); }\n");
    buf_puts(out, "    if (tur_future_done(f)) {\n");
    buf_puts(out, "        if (f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return f->value;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Future not ready */\n");
    buf_puts(out, "    if (!tur_current_fiber) {\n");
    buf_puts(out, "        /* Not in a fiber - run the scheduler until future is done */\n");
    buf_puts(out, "        if (!tur_scheduler) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: no scheduler and not in fiber\\n\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        while (!tur_future_done(f)) {\n");
    buf_puts(out, "            tur_scheduler_run_one(tur_scheduler);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        /* In a fiber - yield and let scheduler resume us */\n");
    buf_puts(out, "        /* Register a callback to re-enqueue this fiber when future completes */\n");
    buf_puts(out, "        f->on_complete.fn = (void (*)(TurFuture *, int64_t))tur_fiber_block_resume;\n");
    buf_puts(out, "        f->on_complete.env = (void *)tur_current_fiber;\n");
    buf_puts(out, "        tur_fiber_block_yield(0);\n");
    buf_puts(out, "        /* When we resume, the future should be done */\n");
    buf_puts(out, "        if (tur_future_done(f) && f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return f->value;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "        fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return f->value;\n");
    buf_puts(out, "}\n\n");
    
    /* Free a future */
    buf_puts(out, "static void tur_future_free(TurFuture *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    free(f);\n");
    buf_puts(out, "}\n\n");
    
    /* Keep old TurAsyncTask for backward compatibility */
    buf_puts(out, "/* Backward compatibility: TurAsyncTask = TurFuture */\n");
    buf_puts(out, "typedef TurFuture TurAsyncTask;\n\n");

    /* Phase SEL0: select waiter infrastructure for fair multi-channel blocking */
    buf_puts(out, "/* Phase SEL0: TurSelectWaiter -- select waiter for fair multi-channel blocking */\n");
    buf_puts(out, "typedef struct TurSelectWaiter TurSelectWaiter;\n");
    buf_puts(out, "struct TurSelectWaiter {\n");
    buf_puts(out, "    pthread_mutex_t *wakeup_mutex;\n");
    buf_puts(out, "    pthread_cond_t  *wakeup_cond;\n");
    buf_puts(out, "    volatile int    *selected_idx;\n");
    buf_puts(out, "    int              clause_idx;\n");
    buf_puts(out, "    TurSelectWaiter *next;\n");
    buf_puts(out, "};\n\n");

    buf_puts(out, "/* Phase SEL0: signal the first unselected waiter in the list */\n");
    buf_puts(out, "static void tur_waiter_signal_one(void *waiter_list) {\n");
    buf_puts(out, "    TurSelectWaiter *w = (TurSelectWaiter *)waiter_list;\n");
    buf_puts(out, "    while (w) {\n");
    buf_puts(out, "        int exp = -1;\n");
    buf_puts(out, "        if (__atomic_compare_exchange_n(w->selected_idx, &exp, w->clause_idx, 0,\n");
    buf_puts(out, "                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {\n");
    buf_puts(out, "            pthread_mutex_lock(w->wakeup_mutex);\n");
    buf_puts(out, "            pthread_cond_signal(w->wakeup_cond);\n");
    buf_puts(out, "            pthread_mutex_unlock(w->wakeup_mutex);\n");
    buf_puts(out, "            return;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        w = w->next;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    /* Phase SEL1: tur_select_blocking -- fair multi-channel blocking select */
    buf_puts(out, "/* Phase SEL1: TurSelectClause -- clause descriptor for tur_select_blocking */\n");
    buf_puts(out, "typedef struct { void *chan; int op; int64_t val; } TurSelectClause;\n\n");
    /* Persistent xorshift32 state: static so it survives across calls (including
     * tail-call-optimised loops) and accumulates entropy across the program. */
    buf_puts(out, "/* Phase SEL1: persistent xorshift32 PRNG for fair select */\n");
    buf_puts(out, "static volatile uint32_t __tur_xr_state = 1u;\n\n");

    buf_puts(out, "/* Phase SEL1: tur_select_blocking -- block on all clauses; wake on first ready.\n");
    buf_puts(out, " * clauses: array of TurSelectClause (op 0=recv, 1=send)\n");
    buf_puts(out, " * n: number of clauses\n");
    buf_puts(out, " * has_default: 1 if a :default arm is present\n");
    buf_puts(out, " * Returns: index of the clause that fired, or -1 for default */\n");
    buf_puts(out, "static int tur_select_blocking(TurSelectClause *clauses, int n, int has_default) {\n");
    buf_puts(out, "    typedef struct {\n");
    buf_puts(out, "        pthread_mutex_t lock; pthread_cond_t not_full; pthread_cond_t not_empty;\n");
    buf_puts(out, "        int64_t *buf; int64_t head; int64_t tail; int64_t count; int64_t cap;\n");
    buf_puts(out, "        void *recv_waiters; void *send_waiters;\n");
    buf_puts(out, "    } ChanBlock_;\n");
    buf_puts(out, "    if (n <= 0) return -2;\n");
    /* Phase 1: non-blocking scan */
    buf_puts(out, "    /* Phase 1: non-blocking scan -- try all clauses without blocking */\n");
    buf_puts(out, "    /* Collect unique channels for lock ordering */\n");
    buf_puts(out, "    ChanBlock_ *lock_order[64];\n");
    buf_puts(out, "    int n_unique = 0;\n");
    buf_puts(out, "    for (int i = 0; i < n && n_unique < 64; i++) {\n");
    buf_puts(out, "        ChanBlock_ *ch = (ChanBlock_ *)clauses[i].chan;\n");
    buf_puts(out, "        int found = 0;\n");
    buf_puts(out, "        for (int j = 0; j < n_unique; j++) if (lock_order[j] == ch) { found = 1; break; }\n");
    buf_puts(out, "        if (!found) lock_order[n_unique++] = ch;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Sort channels by address for deadlock-free locking order */\n");
    buf_puts(out, "    for (int i = 0; i < n_unique - 1; i++)\n");
    buf_puts(out, "        for (int j = i + 1; j < n_unique; j++)\n");
    buf_puts(out, "            if ((uintptr_t)lock_order[i] > (uintptr_t)lock_order[j]) {\n");
    buf_puts(out, "                ChanBlock_ *tmp = lock_order[i]; lock_order[i] = lock_order[j]; lock_order[j] = tmp;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "    /* Acquire all locks */\n");
    buf_puts(out, "    for (int i = 0; i < n_unique; i++) pthread_mutex_lock(&lock_order[i]->lock);\n");
    buf_puts(out, "    /* Final non-blocking scan under all locks */\n");
    buf_puts(out, "    int ready[64]; int n_ready = 0;\n");
    buf_puts(out, "    for (int i = 0; i < n; i++) {\n");
    buf_puts(out, "        ChanBlock_ *ch = (ChanBlock_ *)clauses[i].chan;\n");
    buf_puts(out, "        if (clauses[i].op == 0) { if (ch->count > 0) ready[n_ready++] = i; }\n");
    buf_puts(out, "        else                   { if (ch->count < ch->cap) ready[n_ready++] = i; }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (n_ready > 0) {\n");
    buf_puts(out, "        /* Fair: pick uniformly at random among ready clauses.\n");
    buf_puts(out, "         * Use a persistent xorshift32 state so repeated calls from the\n");
    buf_puts(out, "         * same site (e.g. a TCO loop) don't always produce the same winner. */\n");
    buf_puts(out, "        uint32_t xr = __tur_xr_state;\n");
    buf_puts(out, "        xr ^= (uint32_t)(uintptr_t)clauses;\n");
    buf_puts(out, "        if (!xr) xr = 0x9e3779b9u;\n");
    buf_puts(out, "        xr ^= xr << 13; xr ^= xr >> 17; xr ^= xr << 5;\n");
    buf_puts(out, "        __tur_xr_state = xr;\n");
    buf_puts(out, "        int winner = ready[xr % (uint32_t)n_ready];\n");
    buf_puts(out, "        ChanBlock_ *wch = (ChanBlock_ *)clauses[winner].chan;\n");
    buf_puts(out, "        if (clauses[winner].op == 0) { /* recv */\n");
    buf_puts(out, "            clauses[winner].val = wch->buf[wch->head];\n");
    buf_puts(out, "            wch->head = (wch->head + 1) % wch->cap; wch->count--;\n");
    buf_puts(out, "            pthread_cond_signal(&wch->not_full);\n");
    buf_puts(out, "            tur_waiter_signal_one(wch->send_waiters);\n");
    buf_puts(out, "        } else { /* send */\n");
    buf_puts(out, "            wch->buf[wch->tail] = clauses[winner].val;\n");
    buf_puts(out, "            wch->tail = (wch->tail + 1) % wch->cap; wch->count++;\n");
    buf_puts(out, "            pthread_cond_signal(&wch->not_empty);\n");
    buf_puts(out, "            tur_waiter_signal_one(wch->recv_waiters);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        for (int i = 0; i < n_unique; i++) pthread_mutex_unlock(&lock_order[i]->lock);\n");
    buf_puts(out, "        return winner;\n");
    buf_puts(out, "    }\n");
    /* Phase 2: default arm */
    buf_puts(out, "    /* Phase 2: default arm */\n");
    buf_puts(out, "    if (has_default) {\n");
    buf_puts(out, "        for (int i = 0; i < n_unique; i++) pthread_mutex_unlock(&lock_order[i]->lock);\n");
    buf_puts(out, "        return -1;\n");
    buf_puts(out, "    }\n");
    /* Phase 3: register waiters and sleep */
    buf_puts(out, "    /* Phase 3: register waiters and sleep until one fires */\n");
    buf_puts(out, "    pthread_mutex_t wakeup_mutex;\n");
    buf_puts(out, "    pthread_cond_t  wakeup_cond;\n");
    buf_puts(out, "    pthread_mutex_init(&wakeup_mutex, NULL);\n");
    buf_puts(out, "    pthread_cond_init(&wakeup_cond, NULL);\n");
    buf_puts(out, "    volatile int selected_idx = -1;\n");
    buf_puts(out, "    TurSelectWaiter waiters[64];\n");
    buf_puts(out, "    int wn = n < 64 ? n : 64;\n");
    buf_puts(out, "    for (int i = 0; i < wn; i++) {\n");
    buf_puts(out, "        waiters[i].wakeup_mutex = &wakeup_mutex;\n");
    buf_puts(out, "        waiters[i].wakeup_cond  = &wakeup_cond;\n");
    buf_puts(out, "        waiters[i].selected_idx = &selected_idx;\n");
    buf_puts(out, "        waiters[i].clause_idx   = i;\n");
    buf_puts(out, "        ChanBlock_ *ch = (ChanBlock_ *)clauses[i].chan;\n");
    buf_puts(out, "        if (clauses[i].op == 0) { /* recv waiter */\n");
    buf_puts(out, "            waiters[i].next = (TurSelectWaiter *)ch->recv_waiters;\n");
    buf_puts(out, "            ch->recv_waiters = &waiters[i];\n");
    buf_puts(out, "        } else { /* send waiter */\n");
    buf_puts(out, "            waiters[i].next = (TurSelectWaiter *)ch->send_waiters;\n");
    buf_puts(out, "            ch->send_waiters = &waiters[i];\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Release all channel locks */\n");
    buf_puts(out, "    for (int i = 0; i < n_unique; i++) pthread_mutex_unlock(&lock_order[i]->lock);\n");
    buf_puts(out, "    /* Sleep until woken by a channel operation or cancelled (TC1) */\n");
    buf_puts(out, "    pthread_mutex_lock(&wakeup_mutex);\n");
    buf_puts(out, "    while (selected_idx == -1) {\n");
    buf_puts(out, "        if (tur_thread_cancel_requested()) {\n");
    buf_puts(out, "            pthread_mutex_unlock(&wakeup_mutex);\n");
    buf_puts(out, "            /* Deregister all waiters before cancelling */\n");
    buf_puts(out, "            for (int __ci = 0; __ci < wn; __ci++) {\n");
    buf_puts(out, "                ChanBlock_ *__cch = (ChanBlock_ *)clauses[__ci].chan;\n");
    buf_puts(out, "                pthread_mutex_lock(&__cch->lock);\n");
    buf_puts(out, "                if (clauses[__ci].op == 0) {\n");
    buf_puts(out, "                    TurSelectWaiter **pp = (TurSelectWaiter **)&__cch->recv_waiters;\n");
    buf_puts(out, "                    while (*pp && *pp != &waiters[__ci]) pp = &(*pp)->next;\n");
    buf_puts(out, "                    if (*pp) *pp = (*pp)->next;\n");
    buf_puts(out, "                } else {\n");
    buf_puts(out, "                    TurSelectWaiter **pp = (TurSelectWaiter **)&__cch->send_waiters;\n");
    buf_puts(out, "                    while (*pp && *pp != &waiters[__ci]) pp = &(*pp)->next;\n");
    buf_puts(out, "                    if (*pp) *pp = (*pp)->next;\n");
    buf_puts(out, "                }\n");
    buf_puts(out, "                pthread_mutex_unlock(&__cch->lock);\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            pthread_mutex_destroy(&wakeup_mutex);\n");
    buf_puts(out, "            pthread_cond_destroy(&wakeup_cond);\n");
    buf_puts(out, "            tur_thread_do_cancel();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        struct timespec __sel_ts;\n");
    buf_puts(out, "        clock_gettime(CLOCK_REALTIME, &__sel_ts);\n");
    buf_puts(out, "        long __sel_ns = __sel_ts.tv_nsec + 5000000L;\n");
    buf_puts(out, "        __sel_ts.tv_sec += __sel_ns / 1000000000L;\n");
    buf_puts(out, "        __sel_ts.tv_nsec = __sel_ns % 1000000000L;\n");
    buf_puts(out, "        pthread_cond_timedwait(&wakeup_cond, &wakeup_mutex, &__sel_ts);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    int winner = selected_idx;\n");
    buf_puts(out, "    pthread_mutex_unlock(&wakeup_mutex);\n");
    buf_puts(out, "    /* Deregister all waiters */\n");
    buf_puts(out, "    for (int i = 0; i < wn; i++) {\n");
    buf_puts(out, "        ChanBlock_ *ch = (ChanBlock_ *)clauses[i].chan;\n");
    buf_puts(out, "        pthread_mutex_lock(&ch->lock);\n");
    buf_puts(out, "        if (clauses[i].op == 0) {\n");
    buf_puts(out, "            TurSelectWaiter **pp = (TurSelectWaiter **)&ch->recv_waiters;\n");
    buf_puts(out, "            while (*pp && *pp != &waiters[i]) pp = &(*pp)->next;\n");
    buf_puts(out, "            if (*pp) *pp = (*pp)->next;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            TurSelectWaiter **pp = (TurSelectWaiter **)&ch->send_waiters;\n");
    buf_puts(out, "            while (*pp && *pp != &waiters[i]) pp = &(*pp)->next;\n");
    buf_puts(out, "            if (*pp) *pp = (*pp)->next;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        pthread_mutex_unlock(&ch->lock);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Execute the winning clause */\n");
    buf_puts(out, "    ChanBlock_ *wch = (ChanBlock_ *)clauses[winner].chan;\n");
    buf_puts(out, "    pthread_mutex_lock(&wch->lock);\n");
    buf_puts(out, "    if (clauses[winner].op == 0) { /* recv */\n");
    buf_puts(out, "        while (wch->count == 0) pthread_cond_wait(&wch->not_empty, &wch->lock);\n");
    buf_puts(out, "        clauses[winner].val = wch->buf[wch->head];\n");
    buf_puts(out, "        wch->head = (wch->head + 1) % wch->cap; wch->count--;\n");
    buf_puts(out, "        pthread_cond_signal(&wch->not_full);\n");
    buf_puts(out, "        tur_waiter_signal_one(wch->send_waiters);\n");
    buf_puts(out, "    } else { /* send */\n");
    buf_puts(out, "        while (wch->count == wch->cap) pthread_cond_wait(&wch->not_full, &wch->lock);\n");
    buf_puts(out, "        wch->buf[wch->tail] = clauses[winner].val;\n");
    buf_puts(out, "        wch->tail = (wch->tail + 1) % wch->cap; wch->count++;\n");
    buf_puts(out, "        pthread_cond_signal(&wch->not_empty);\n");
    buf_puts(out, "        tur_waiter_signal_one(wch->recv_waiters);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    pthread_mutex_unlock(&wch->lock);\n");
    buf_puts(out, "    pthread_mutex_destroy(&wakeup_mutex);\n");
    buf_puts(out, "    pthread_cond_destroy(&wakeup_cond);\n");
    buf_puts(out, "    return winner;\n");
    buf_puts(out, "}\n\n");

    /* global_effect_handler_chain is declared earlier, before tur_fiber_block_new */
    buf_puts(out, "static int64_t tur_effect_perform(const char *name, int64_t *args, int n_args) {\n");
        /* T21-B: use fiber-local handler chain when inside a fiber */
    buf_puts(out, "    EffectHandlerFrame *frame =\n");
    buf_puts(out, "        (tur_current_fiber && tur_current_fiber->effect_handler_chain)\n");
    buf_puts(out, "        ? (EffectHandlerFrame *)tur_current_fiber->effect_handler_chain\n");
    buf_puts(out, "        : global_effect_handler_chain;\n");
    buf_puts(out, "    while (frame) {\n");
    buf_puts(out, "        for (int __i = 0; __i < frame->n_cases; __i++) {\n");
    buf_puts(out, "            if (strcmp(frame->cases[__i].effect_name, name) == 0) {\n");
    /* Phase 19D: intercept case - handler_fn == NULL means fiber-yield to parent dispatch loop */
    buf_puts(out, "                if (frame->cases[__i].handler_fn == NULL) {\n");
    buf_puts(out, "                    /* Phase 19D: intercept case - yield fiber to parent dispatch loop */\n");
    buf_puts(out, "                    FiberBlock *__cur = tur_current_fiber;\n");
    buf_puts(out, "                    if (!__cur || !__cur->eff_ctx) { fprintf(stderr, \"Unhandled effect: %s\\n\", name); abort(); }\n");
    buf_puts(out, "                    TurEffectCaptureCtx *__cap = (TurEffectCaptureCtx *)__cur->eff_ctx;\n");
    buf_puts(out, "                    __cap->eff_name = name;\n");
    buf_puts(out, "                    int __cn = n_args < 8 ? n_args : 8;\n");
    buf_puts(out, "                    for (int __ai = 0; __ai < __cn; __ai++) __cap->eff_args[__ai] = args[__ai];\n");
    buf_puts(out, "                    __cap->eff_n_args = n_args;\n");
    buf_puts(out, "                    __cap->has_pending_effect = true;\n");
    buf_puts(out, "                    tur_fiber_block_yield(0);\n");
    buf_puts(out, "                    __cap->has_pending_effect = false;\n");
    buf_puts(out, "                    return __cur->arg;\n");
    buf_puts(out, "                }\n");
    /* Phase P19-5: allocate a fresh TurContK on the stack per invocation */
    buf_puts(out, "                TurContK __fresh_k = {false, tur_current_fiber};\n");
    buf_puts(out, "                return frame->cases[__i].handler_fn(args, n_args, (int64_t)(intptr_t)&__fresh_k, frame->cases[__i].env);\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        frame = frame->parent;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    fprintf(stderr, \"Unhandled effect: %s\\n\", name);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n\n");

    /* Phase 9: Emit rc.h inline for rc<T> + weak<T> support */
    /* Phase 10: GC color enum and runtime (needed by rc_cb_alloc) */
    buf_puts(out, "/* rc<T> + weak<T> reference counting - Phase 9 */\n");
    buf_puts(out, "/* Phase 10: GC color enum for Bacon-Rajan */\n");
    buf_puts(out, "typedef enum { GC_WHITE, GC_GREY, GC_BLACK, GC_PURPLE } GcColor;\n\n");
    buf_puts(out, "typedef void (*RcDropFn)(void *value);\n\n");
    buf_puts(out, "typedef struct RcControlBlock RcControlBlock;\n\n");
    buf_puts(out, "struct RcControlBlock {\n");
    buf_puts(out, "    uint64_t strong_count;\n");
    buf_puts(out, "    uint64_t weak_count;\n");
    buf_puts(out, "    void *value;\n");
    buf_puts(out, "    RcDropFn drop_fn;\n");
    buf_puts(out, "    uint8_t value_type_kind;\n");
    /* Phase 10: Bacon-Rajan GC fields */
    buf_puts(out, "    uint8_t color;           /* GC color */\n");
    buf_puts(out, "    bool may_contain_cycles;  /* Hint for GC */\n");
    buf_puts(out, "    uint8_t reserved[6];\n");
    buf_puts(out, "};\n\n");
    /* Phase 9: Deferred free queue to avoid deep recursion in rc_strong_decrement */
    buf_puts(out, "#define RC_FREE_QUEUE_CAPACITY 65536\n");
    buf_puts(out, "static RcControlBlock *rc_free_queue[RC_FREE_QUEUE_CAPACITY];\n");
    buf_puts(out, "static uint32_t rc_free_queue_count = 0;\n");
    buf_puts(out, "static uint32_t rc_free_queue_drain(void);  /* Forward decl */\n");
    buf_puts(out, "static void rc_free_queue_push(RcControlBlock *cb);  /* Forward decl */\n");
    buf_puts(out, "bool rc_strong_decrement(RcControlBlock *cb);  /* Forward decl */\n");
    buf_puts(out, "bool rc_weak_decrement(RcControlBlock *cb);    /* Forward decl */\n");
    buf_puts(out, "static void rc_free_queue_push(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    if (rc_free_queue_count >= RC_FREE_QUEUE_CAPACITY) {\n");
    buf_puts(out, "        fprintf(stderr, \"rc_free_queue: full, aborting\\n\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    rc_free_queue[rc_free_queue_count++] = cb;\n");
    buf_puts(out, "}\n\n");
    /* Phase 10: GC globals and helper functions (needed before rc_cb_alloc) */
    buf_puts(out, "#define GC_GLOBAL_REGISTRY_CAPACITY 4096\n");
    buf_puts(out, "static RcControlBlock *gc_all_blocks[GC_GLOBAL_REGISTRY_CAPACITY];\n");
    buf_puts(out, "static uint32_t gc_all_blocks_count = 0;\n\n");
    /* GC state must be declared early because gc_on_strong_decrement (called from
     * rc_strong_decrement) needs it.  The collector functions themselves are
     * defined later, after all RC helpers. */
    buf_puts(out, "#define GC_SUSPECT_THRESHOLD 128\n");
    buf_puts(out, "#define GC_MAX_SUSPECTS 4096\n\n");
    buf_puts(out, "typedef enum { GC_DISABLED, GC_MANUAL, GC_THRESHOLD } GcMode;\n\n");
    buf_puts(out, "static RcControlBlock *gc_suspect_roots[GC_MAX_SUSPECTS];\n");
    buf_puts(out, "static uint32_t gc_suspect_count = 0;\n");
    buf_puts(out, "static RcControlBlock *gc_grey_queue[GC_MAX_SUSPECTS];\n");
    buf_puts(out, "static uint32_t gc_grey_count = 0;\n");
    buf_puts(out, "static GcMode gc_mode = GC_DISABLED;\n");
    buf_puts(out, "static bool gc_enabled = false;\n\n");
    buf_puts(out, "static void gc_collect(void);  /* Forward decl */\n\n");
    buf_puts(out, "static void gc_set_color(RcControlBlock *cb, GcColor color) {\n");
    buf_puts(out, "    if (cb) cb->color = color;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static GcColor gc_get_color(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (cb) return cb->color;\n");
    buf_puts(out, "    return GC_WHITE;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_register_block(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb || gc_all_blocks_count >= GC_GLOBAL_REGISTRY_CAPACITY) return;\n");
    buf_puts(out, "    gc_all_blocks[gc_all_blocks_count++] = cb;\n");
    buf_puts(out, "    cb->color = GC_WHITE;\n");
    buf_puts(out, "    cb->may_contain_cycles = true;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_unregister_block(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        if (gc_all_blocks[i] == cb) {\n");
    buf_puts(out, "            gc_all_blocks[i] = gc_all_blocks[gc_all_blocks_count - 1];\n");
    buf_puts(out, "            gc_all_blocks_count--;\n");
    buf_puts(out, "            break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    /* Phase 10: Suspect buffer management (before rc_strong_decrement) */
    buf_puts(out, "static void gc_add_suspect(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb || !gc_enabled || gc_mode == GC_DISABLED) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_suspect_count; i++) {\n");
    buf_puts(out, "        if (gc_suspect_roots[i] == cb) return;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (gc_suspect_count >= GC_MAX_SUSPECTS) return;\n");
    buf_puts(out, "    gc_suspect_roots[gc_suspect_count++] = cb;\n");
    buf_puts(out, "    cb->color = GC_PURPLE;\n");
    buf_puts(out, "    /* Threshold mode: auto-collect when buffer is full */\n");
    buf_puts(out, "    if (gc_mode == GC_THRESHOLD && gc_suspect_count >= GC_SUSPECT_THRESHOLD) {\n");
    buf_puts(out, "        gc_collect();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_remove_suspect(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_suspect_count; i++) {\n");
    buf_puts(out, "        if (gc_suspect_roots[i] == cb) {\n");
    buf_puts(out, "            gc_suspect_roots[i] = gc_suspect_roots[gc_suspect_count - 1];\n");
    buf_puts(out, "            gc_suspect_count--;\n");
    buf_puts(out, "            return;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_on_strong_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    /* Zombie: strong reached 0 but weak refs still exist */\n");
    buf_puts(out, "    if (cb->weak_count > 0) {\n");
    buf_puts(out, "        gc_add_suspect(cb);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "\n/* Phase 9: Deferred free queue drain */\n");
    buf_puts(out, "static uint32_t rc_free_queue_drain(void) {\n");
    buf_puts(out, "    if (rc_free_queue_count == 0) return 0;\n");
    buf_puts(out, "    uint32_t freed = 0;\n");
    buf_puts(out, "    while (rc_free_queue_count > 0) {\n");
    buf_puts(out, "        RcControlBlock *cb = rc_free_queue[0];\n");
    buf_puts(out, "        memmove(rc_free_queue, rc_free_queue + 1,\n");
    buf_puts(out, "                (rc_free_queue_count - 1) * sizeof(RcControlBlock *));\n");
    buf_puts(out, "        rc_free_queue_count--;\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    buf_puts(out, "        if (cb->value) cb->drop_fn(cb->value);\n");
    buf_puts(out, "        free(cb);\n");
    buf_puts(out, "        freed++;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return freed;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void default_rc_drop_fn(void *value) {\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_ref_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    void *inner = *((void **)value);\n");
    buf_puts(out, "    if (inner) free(inner);\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_rc_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    RcControlBlock *inner = *((RcControlBlock **)value);\n");
    buf_puts(out, "    if (inner) { rc_strong_decrement(inner); rc_free_queue_drain(); }\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_weak_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    RcControlBlock *inner = *((RcControlBlock **)value);\n");
    buf_puts(out, "    if (inner) rc_weak_decrement(inner);\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static RcDropFn default_drop_fn_for_type(int value_type_kind) {\n");
    buf_puts(out, "    switch (value_type_kind) {\n");
    buf_puts(out, "        case 8: return drop_ref_payload;   /* TY_REF */\n");
    buf_puts(out, "        case 9: return drop_rc_payload;    /* TY_RC */\n");
    buf_puts(out, "        case 10: return drop_weak_payload; /* TY_WEAK */\n");
    buf_puts(out, "        default: return default_rc_drop_fn;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *rc_cb_alloc(size_t value_size, int value_type_kind, RcDropFn drop_fn) {\n");
    buf_puts(out, "    size_t total_size = sizeof(RcControlBlock) + value_size;\n");
    buf_puts(out, "    RcControlBlock *cb = (RcControlBlock *)malloc(total_size);\n");
    buf_puts(out, "    if (!cb) { fprintf(stderr, \"rc: out of memory\\n\"); abort(); }\n");
    buf_puts(out, "    cb->strong_count = 1;\n");
    buf_puts(out, "    cb->weak_count = 0;\n");
    buf_puts(out, "    cb->value = (void *)(cb + 1);\n");
    buf_puts(out, "    cb->drop_fn = drop_fn ? drop_fn : default_drop_fn_for_type(value_type_kind);\n");
    buf_puts(out, "    cb->value_type_kind = value_type_kind;\n");
    buf_puts(out, "    memset(cb->reserved, 0, sizeof(cb->reserved));\n");
    buf_puts(out, "    /* Register with GC; primitives (type_kind<=7) cannot form cycles */\n");
    buf_puts(out, "    gc_register_block(cb);\n");
    buf_puts(out, "    if (value_type_kind <= 7) cb->may_contain_cycles = false;\n");
    buf_puts(out, "    return cb;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_strong_increment(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return ++cb->strong_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_strong_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    cb->strong_count--;\n");
    buf_puts(out, "    if (cb->strong_count == 0) {\n");
    buf_puts(out, "        if (cb->weak_count > 0) {\n");
    buf_puts(out, "            gc_on_strong_decrement(cb);\n");
    buf_puts(out, "            return false;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            rc_free_queue_push(cb);\n");
    buf_puts(out, "            return true;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_weak_increment(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return ++cb->weak_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_weak_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    cb->weak_count--;\n");
    buf_puts(out, "    if (cb->weak_count == 0 && cb->strong_count == 0) {\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    buf_puts(out, "        /* Free zombie value if GC did not already collect it */\n");
    buf_puts(out, "        if (cb->value && cb->drop_fn) {\n");
    buf_puts(out, "            cb->drop_fn(cb->value);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        free(cb);\n");
    buf_puts(out, "        return true;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_strong_count(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return cb->strong_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_weak_count(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return cb->weak_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_is_alive(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    return cb->strong_count > 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *rc_upgrade(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    if (cb->strong_count > 0) {\n");
    buf_puts(out, "        rc_strong_increment(cb);\n");
    buf_puts(out, "        return cb;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "void *rc_get_value(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    return cb->value;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *tur_rc_from_ref(void *ref_value, int value_type_kind) {\n");
    buf_puts(out, "    if (!ref_value) return NULL;\n");
    buf_puts(out, "    RcControlBlock *cb = (RcControlBlock *)malloc(sizeof(RcControlBlock));\n");
    buf_puts(out, "    if (!cb) { fprintf(stderr, \"rc/from-ref: out of memory\\n\"); abort(); }\n");
    buf_puts(out, "    cb->strong_count = 1;\n");
    buf_puts(out, "    cb->weak_count = 0;\n");
    buf_puts(out, "    cb->value = ref_value;\n");
    buf_puts(out, "    cb->drop_fn = default_drop_fn_for_type(value_type_kind);\n");
    buf_puts(out, "    cb->value_type_kind = (uint8_t)value_type_kind;\n");
    buf_puts(out, "    cb->color = GC_WHITE;\n");
    buf_puts(out, "    cb->may_contain_cycles = true;\n");
    buf_puts(out, "    memset(cb->reserved, 0, sizeof(cb->reserved));\n");
    buf_puts(out, "    gc_register_block(cb);\n");
    buf_puts(out, "    return cb;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "void *tur_ref_from_rc(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    if (cb->strong_count != 1 || cb->weak_count != 0) {\n");
    buf_puts(out, "        fprintf(stderr, \"ref/from-rc requires unique rc (strong_count==1 and weak_count==0), got strong=%llu weak=%llu\\n\",\n");
    buf_puts(out, "                (unsigned long long)cb->strong_count, (unsigned long long)cb->weak_count);\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    void *value = cb->value;\n");
    buf_puts(out, "    cb->value = NULL;\n");
    buf_puts(out, "    gc_unregister_block(cb);\n");
    buf_puts(out, "    free(cb);\n");
    buf_puts(out, "    return value;\n");
    buf_puts(out, "}\n\n");
    
    /* Phase 10: Emit remaining GC runtime — mark + trial deletion phases */
    buf_puts(out, "/* gc (Bacon-Rajan cycle collector - trial deletion) - Phase 10 */\n");
    buf_puts(out, "static void gc_mark_phase(void) {\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        gc_all_blocks[i]->color = GC_WHITE;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        RcControlBlock *cb = gc_all_blocks[i];\n");
    buf_puts(out, "        if (cb->strong_count > 0) {\n");
    buf_puts(out, "            cb->color = GC_BLACK;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    /* Trial deletion: free zombie suspects not reachable from strong roots */
    buf_puts(out, "static void gc_trial_deletion_phase(void) {\n");
    buf_puts(out, "    uint32_t i = 0;\n");
    buf_puts(out, "    while (i < gc_suspect_count) {\n");
    buf_puts(out, "        RcControlBlock *cb = gc_suspect_roots[i];\n");
    buf_puts(out, "        /* If revived or reachable from strong roots, keep */\n");
    buf_puts(out, "        if (cb->strong_count > 0 || cb->color == GC_BLACK) {\n");
    buf_puts(out, "            cb->color = GC_WHITE;\n");
    buf_puts(out, "            i++;\n");
    buf_puts(out, "            continue;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* WHITE and strong_count == 0: zombie — free value, keep cb for weak refs */\n");
    buf_puts(out, "        if (cb->value && cb->drop_fn) {\n");
    buf_puts(out, "            cb->drop_fn(cb->value);\n");
    buf_puts(out, "            cb->value = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* Unregister from global registry (cb stays alive for weak refs) */\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    buf_puts(out, "        /* Remove from suspect buffer without advancing i */\n");
    buf_puts(out, "        gc_suspect_roots[i] = gc_suspect_roots[gc_suspect_count - 1];\n");
    buf_puts(out, "        gc_suspect_count--;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_collect(void) {\n");
    buf_puts(out, "    if (!gc_enabled || gc_mode == GC_DISABLED) return;\n");
    buf_puts(out, "    gc_grey_count = 0;\n");
    buf_puts(out, "    gc_mark_phase();\n");
    buf_puts(out, "    gc_trial_deletion_phase();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_force(void) {\n");
    buf_puts(out, "    gc_collect();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_enable(void) {\n");
    buf_puts(out, "    gc_enabled = true;\n");
    buf_puts(out, "    /* Default to manual mode when enabled */\n");
    buf_puts(out, "    if (gc_mode == GC_DISABLED) gc_mode = GC_MANUAL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_disable(void) {\n");
    buf_puts(out, "    gc_enabled = false;\n");
    buf_puts(out, "    gc_mode = GC_DISABLED;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_set_mode(GcMode mode) {\n");
    buf_puts(out, "    gc_mode = mode;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static bool gc_is_alive(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    if (cb->strong_count > 0) return true;\n");
    buf_puts(out, "    return (cb->color == GC_BLACK || cb->color == GC_GREY);\n");
    buf_puts(out, "}\n\n");

    /* SS2: TurChannel -- synchronous rendezvous channel for session types.
     * Only emitted when -Xsessions is active. */
    if (g_sessions_enabled) {
        buf_puts(out, "/* SS2: TurChannel -- synchronous rendezvous channel for session types */\n");
        buf_puts(out, "#ifndef NDEBUG\n");
        buf_puts(out, "#  define TUR_DBGPROTO(s) (s)\n");
        buf_puts(out, "#else\n");
        buf_puts(out, "#  define TUR_DBGPROTO(s) ((const char*)0)\n");
        buf_puts(out, "#endif\n");
        /* TurSyncCh: synchronous rendezvous slot with explicit send/recv handshake.
         * state: 0=idle, 1=sender deposited (waiting for recv), 2=receiver acked (waiting for send to return)
         * cv: broadcast on every state change; both sender and receiver use it.
         * The three-state design ensures no ABA races when the same thread
         * receives a value and immediately sends a new one on the same channel. */
        buf_puts(out, "typedef struct {\n");
        buf_puts(out, "    pthread_mutex_t mu;\n");
        buf_puts(out, "    pthread_cond_t  cv;\n");
        buf_puts(out, "    int64_t val;\n");
        buf_puts(out, "    int state; /* 0=idle 1=data-ready 2=data-acked */\n");
        buf_puts(out, "} TurSyncCh;\n");
        buf_puts(out, "typedef struct {\n");
        buf_puts(out, "    TurSyncCh data;\n");
        buf_puts(out, "    TurSyncCh branch;\n");
        buf_puts(out, "    int refcount;\n");
        buf_puts(out, "    int abandoned; /* set when timeout fires; unblocks sender */\n");
        buf_puts(out, "    pthread_mutex_t rc_mu;\n");
        buf_puts(out, "#ifndef NDEBUG\n");
        buf_puts(out, "    const char *dbg_proto;\n");
        buf_puts(out, "#endif\n");
        buf_puts(out, "} TurChannel;\n");
        buf_puts(out, "static TurChannel *tur_session_new(const char *proto) {\n");
        buf_puts(out, "    TurChannel *ch = (TurChannel *)calloc(1, sizeof(TurChannel));\n");
        buf_puts(out, "    pthread_mutex_init(&ch->data.mu, NULL);\n");
        buf_puts(out, "    pthread_cond_init(&ch->data.cv, NULL);\n");
        buf_puts(out, "    pthread_mutex_init(&ch->branch.mu, NULL);\n");
        buf_puts(out, "    pthread_cond_init(&ch->branch.cv, NULL);\n");
        buf_puts(out, "    pthread_mutex_init(&ch->rc_mu, NULL);\n");
        buf_puts(out, "    ch->refcount = 2;\n");
        buf_puts(out, "#ifndef NDEBUG\n");
        buf_puts(out, "    ch->dbg_proto = proto;\n");
        buf_puts(out, "#else\n");
        buf_puts(out, "    (void)proto;\n");
        buf_puts(out, "#endif\n");
        buf_puts(out, "    return ch;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_session_send(TurChannel *ch, int64_t val) {\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->data.mu);\n");
        buf_puts(out, "    /* phase 1: wait for idle slot, deposit value */\n");
        buf_puts(out, "    while (ch->data.state != 0 && !ch->abandoned)\n");
        buf_puts(out, "        pthread_cond_wait(&ch->data.cv, &ch->data.mu);\n");
        buf_puts(out, "    if (ch->abandoned) { pthread_mutex_unlock(&ch->data.mu); return; }\n");
        buf_puts(out, "    ch->data.val = val; ch->data.state = 1;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "    /* phase 2: wait for receiver ack (state==2), then clear to idle */\n");
        buf_puts(out, "    while (ch->data.state != 2 && !ch->abandoned)\n");
        buf_puts(out, "        pthread_cond_wait(&ch->data.cv, &ch->data.mu);\n");
        buf_puts(out, "    if (!ch->abandoned) {\n");
        buf_puts(out, "        ch->data.state = 0;\n");
        buf_puts(out, "        pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->data.mu);\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static int64_t tur_session_recv(TurChannel *ch) {\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->data.mu);\n");
        buf_puts(out, "    /* wait for data-ready (state==1), read, ack */\n");
        buf_puts(out, "    while (ch->data.state != 1) pthread_cond_wait(&ch->data.cv, &ch->data.mu);\n");
        buf_puts(out, "    int64_t v = ch->data.val; ch->data.state = 2;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->data.mu);\n");
        buf_puts(out, "    return v;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_session_send_tag(TurChannel *ch, int64_t tag) {\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->branch.mu);\n");
        buf_puts(out, "    while (ch->branch.state != 0) pthread_cond_wait(&ch->branch.cv, &ch->branch.mu);\n");
        buf_puts(out, "    ch->branch.val = tag; ch->branch.state = 1;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->branch.cv);\n");
        buf_puts(out, "    while (ch->branch.state != 2) pthread_cond_wait(&ch->branch.cv, &ch->branch.mu);\n");
        buf_puts(out, "    ch->branch.state = 0;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->branch.cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->branch.mu);\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static int64_t tur_session_recv_tag(TurChannel *ch) {\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->branch.mu);\n");
        buf_puts(out, "    while (ch->branch.state != 1) pthread_cond_wait(&ch->branch.cv, &ch->branch.mu);\n");
        buf_puts(out, "    int64_t tag = ch->branch.val; ch->branch.state = 2;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->branch.cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->branch.mu);\n");
        buf_puts(out, "    return tag;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_session_close(TurChannel *ch) {\n");
        buf_puts(out, "    /* signal abandoned so any blocked sender/sender_tag wakes up */\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->data.mu);\n");
        buf_puts(out, "    ch->abandoned = 1;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->data.mu);\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->rc_mu);\n");
        buf_puts(out, "    int rc = --ch->refcount;\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->rc_mu);\n");
        buf_puts(out, "    if (rc == 0) {\n");
        buf_puts(out, "        pthread_mutex_destroy(&ch->data.mu); pthread_cond_destroy(&ch->data.cv);\n");
        buf_puts(out, "        pthread_mutex_destroy(&ch->branch.mu); pthread_cond_destroy(&ch->branch.cv);\n");
        buf_puts(out, "        pthread_mutex_destroy(&ch->rc_mu); free(ch);\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void *tur_session_thread_wrapper(void *arg) {\n");
        buf_puts(out, "    int64_t *fat = (int64_t *)arg;\n");
        buf_puts(out, "    int64_t (*thunk)(void *) = (int64_t (*)(void *))(intptr_t)fat[0];\n");
        buf_puts(out, "    thunk(arg);\n");
        buf_puts(out, "    return NULL;\n");
        buf_puts(out, "}\n");
        /* SS3c: thread-local storage for recv-timeout value. */
        buf_puts(out, "static _Thread_local int64_t tur__rtv_ = 0;\n");
        buf_puts(out, "static int64_t tur_session_recv_timeout(TurChannel *ch, int64_t ms) {\n");
        buf_puts(out, "    struct timespec ts;\n");
        buf_puts(out, "    clock_gettime(CLOCK_REALTIME, &ts);\n");
        buf_puts(out, "    ts.tv_sec  += (time_t)(ms / 1000);\n");
        buf_puts(out, "    ts.tv_nsec += (long)((ms % 1000) * 1000000L);\n");
        buf_puts(out, "    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->data.mu);\n");
        buf_puts(out, "    int rc = 0;\n");
        buf_puts(out, "    while (ch->data.state != 1 && rc == 0)\n");
        buf_puts(out, "        rc = pthread_cond_timedwait(&ch->data.cv, &ch->data.mu, &ts);\n");
        buf_puts(out, "    int64_t tag;\n");
        buf_puts(out, "    if (ch->data.state == 1) {\n");
        buf_puts(out, "        tur__rtv_ = ch->data.val; ch->data.state = 2;\n");
        buf_puts(out, "        pthread_cond_broadcast(&ch->data.cv);\n");
        buf_puts(out, "        tag = 0;\n");
        buf_puts(out, "    } else {\n");
        buf_puts(out, "        tag = 1;\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->data.mu);\n");
        buf_puts(out, "    return tag;\n");
        buf_puts(out, "}\n\n");
        /* SS7: Multi-party session router (TurRouter / TurRole). */
        buf_puts(out, "/* SS7: Multi-party session router.\n");
        buf_puts(out, " * TurRouter: holds N*N TurSyncCh slots (i,j) = slot[i*n_roles+j], i!=j used.\n");
        buf_puts(out, " * refcount = number of live TurRole endpoints; freed when it hits 0. */\n");
        buf_puts(out, "typedef struct {\n");
        buf_puts(out, "    int n_roles;\n");
        buf_puts(out, "    int refcount;\n");
        buf_puts(out, "    pthread_mutex_t rc_mu;\n");
        buf_puts(out, "    TurSyncCh slots[1]; /* flexible: actually n_roles*n_roles entries */\n");
        buf_puts(out, "} TurRouter;\n");
        buf_puts(out, "typedef struct {\n");
        buf_puts(out, "    TurRouter *router;\n");
        buf_puts(out, "    int role_idx;\n");
        buf_puts(out, "} TurRole;\n");
        buf_puts(out, "static TurRouter *tur_router_new(int n) {\n");
        buf_puts(out, "    size_t sz = sizeof(TurRouter) + (size_t)((n * n) - 1) * sizeof(TurSyncCh);\n");
        buf_puts(out, "    TurRouter *r = (TurRouter *)calloc(1, sz);\n");
        buf_puts(out, "    r->n_roles  = n;\n");
        buf_puts(out, "    r->refcount = n;\n");
        buf_puts(out, "    pthread_mutex_init(&r->rc_mu, NULL);\n");
        buf_puts(out, "    for (int i = 0; i < n * n; i++) {\n");
        buf_puts(out, "        pthread_mutex_init(&r->slots[i].mu, NULL);\n");
        buf_puts(out, "        pthread_cond_init(&r->slots[i].cv, NULL);\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "    return r;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void *tur_make_roles(int n, int idx) {\n");
        buf_puts(out, "    TurRouter *r = tur_router_new(n);\n");
        buf_puts(out, "    TurRole *role = (TurRole *)malloc(sizeof(TurRole));\n");
        buf_puts(out, "    role->router   = r;\n");
        buf_puts(out, "    role->role_idx = idx;\n");
        buf_puts(out, "    return (void *)role;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void *tur_get_role(void *base, int peer_idx) {\n");
        buf_puts(out, "    TurRole *b = (TurRole *)base;\n");
        buf_puts(out, "    TurRole *role = (TurRole *)malloc(sizeof(TurRole));\n");
        buf_puts(out, "    role->router   = b->router;\n");
        buf_puts(out, "    role->role_idx = peer_idx;\n");
        buf_puts(out, "    return (void *)role;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_router_send(void *role_ptr, int to_idx, int64_t val) {\n");
        buf_puts(out, "    TurRole *role = (TurRole *)role_ptr;\n");
        buf_puts(out, "    TurSyncCh *ch = &role->router->slots[role->role_idx * role->router->n_roles + to_idx];\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->mu);\n");
        buf_puts(out, "    while (ch->state != 0) pthread_cond_wait(&ch->cv, &ch->mu);\n");
        buf_puts(out, "    ch->val = val; ch->state = 1;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->cv);\n");
        buf_puts(out, "    while (ch->state != 2) pthread_cond_wait(&ch->cv, &ch->mu);\n");
        buf_puts(out, "    ch->state = 0;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->mu);\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static int64_t tur_router_recv(void *role_ptr, int from_idx) {\n");
        buf_puts(out, "    TurRole *role = (TurRole *)role_ptr;\n");
        buf_puts(out, "    TurSyncCh *ch = &role->router->slots[from_idx * role->router->n_roles + role->role_idx];\n");
        buf_puts(out, "    pthread_mutex_lock(&ch->mu);\n");
        buf_puts(out, "    while (ch->state != 1) pthread_cond_wait(&ch->cv, &ch->mu);\n");
        buf_puts(out, "    int64_t v = ch->val; ch->state = 2;\n");
        buf_puts(out, "    pthread_cond_broadcast(&ch->cv);\n");
        buf_puts(out, "    pthread_mutex_unlock(&ch->mu);\n");
        buf_puts(out, "    return v;\n");
        buf_puts(out, "}\n");
        buf_puts(out, "static void tur_role_close(void *role_ptr) {\n");
        buf_puts(out, "    TurRole *role = (TurRole *)role_ptr;\n");
        buf_puts(out, "    pthread_mutex_lock(&role->router->rc_mu);\n");
        buf_puts(out, "    int rc = --role->router->refcount;\n");
        buf_puts(out, "    pthread_mutex_unlock(&role->router->rc_mu);\n");
        buf_puts(out, "    if (rc == 0) {\n");
        buf_puts(out, "        int n = role->router->n_roles;\n");
        buf_puts(out, "        for (int i = 0; i < n * n; i++) {\n");
        buf_puts(out, "            pthread_mutex_destroy(&role->router->slots[i].mu);\n");
        buf_puts(out, "            pthread_cond_destroy(&role->router->slots[i].cv);\n");
        buf_puts(out, "        }\n");
        buf_puts(out, "        pthread_mutex_destroy(&role->router->rc_mu);\n");
        buf_puts(out, "        free(role->router);\n");
        buf_puts(out, "    }\n");
        buf_puts(out, "    free(role);\n");
        buf_puts(out, "}\n\n");
    }

    /* Phase 4 v1: Collect all defer thunks into a buffer so they can be
     * emitted after extern_decls and fwd_decls (defer bodies may call
     * extern-c functions or forward-declared Turmeric functions). */
    emit_pending_defer_thunks(&ctx, &defer_thunks);

    /* Final assembly order (ensures correct C visibility):
     *  1. early_file  - struct typedefs + drop glue (visible to everything)
     *  2. extern_decls - user extern-c declarations
     *  3. fwd_decls   - Turmeric function forward declarations (visible to handlers)
     *  4. defer_thunks - defer body functions (may call extern-c or Turmeric fns)
     *  5. pending_handler_fns - effect handler functions (can call Turmeric fns)
     *  6. file        - Turmeric function definitions (can reference handler fns by name)
     *  7. main()      - entry point body
     */
    if (early_file.len)  { buf_write(out, early_file.data, early_file.len); buf_putc(out, '\n'); }
    if (extern_decls.len){ buf_write(out, extern_decls.data, extern_decls.len); buf_putc(out, '\n'); }
    if (fwd_decls.len)   { buf_write(out, fwd_decls.data, fwd_decls.len); buf_putc(out, '\n'); }
    if (defer_thunks.len){ buf_write(out, defer_thunks.data, defer_thunks.len); buf_putc(out, '\n'); }
    buf_free(&early_file);
    buf_free(&extern_decls);
    buf_free(&fwd_decls);
    buf_free(&defer_thunks);

    /* Phase 19: Effect handler functions (after fwd_decls so they can call
     * user-defined Turmeric functions, before file so fn defs can reference them). */
    if (ctx.pending_handler_fns && ctx.pending_handler_fns->len > 0) {
        buf_write(out, ctx.pending_handler_fns->data, ctx.pending_handler_fns->len);
        buf_free(ctx.pending_handler_fns);
        buf_init(ctx.pending_handler_fns);
    }

    if (file.len) { buf_write(out, file.data, file.len); buf_putc(out, '\n'); }

    if (!user_has_main) {
        /* Only generate main() if user didn't define one */
        buf_puts(out, "int main(int argc, char **argv) {\n");
        /* Phase R6: Set g_panic_trace from compiler flag */
        if (g_panic_trace) {
            buf_puts(out, "    g_panic_trace = 1;\n");
        }
        /* CLI-ARGS: Build *args* list from argv[1..] as a linked list of char* (as int64_t). */
        buf_puts(out, "    /* *args*: build cons list from argv[1..argc-1] */\n");
        buf_puts(out, "    g_tur_args = 0;\n");
        buf_puts(out, "    for (int _ai = argc - 1; _ai >= 1; _ai--) {\n");
        buf_puts(out, "        typedef struct { int64_t value; int64_t next; } __tur_args_cell;\n");
        buf_puts(out, "        __tur_args_cell *_c = (__tur_args_cell *)malloc(sizeof(__tur_args_cell));\n");
        buf_puts(out, "        _c->value = (int64_t)(intptr_t)argv[_ai];\n");
        buf_puts(out, "        _c->next = g_tur_args;\n");
        buf_puts(out, "        g_tur_args = (int64_t)(intptr_t)_c;\n");
        buf_puts(out, "    }\n");
        if (body.len) buf_write(out, body.data, body.len);
        buf_puts(out, "    return 0;\n");
        buf_puts(out, "}\n");
    }

    buf_free(&file);
    buf_free(&body);
    free(items);
    free(ctx.env_struct_names);
    return 0;
}

/* ------------ Phase 2: Multi-file support ------------ */

/* Sanitize a module name for use in C header guards (single underscore for / and -). */
static void sanitize_module_name(char *out, const char *name, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; name[i] && k < cap - 1; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/') {
            out[k++] = (c == '-' || c == '/') ? '_' : c;
        }
    }
    out[k] = '\0';
}

/* Mangle a module name for use as a C file base name / symbol prefix.
 * Uses double underscore for '/' (so geom/vector → geom__vector) and
 * single underscore for '-'. */
static void mangle_module_name(char *out, const char *name, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; name[i] && k < cap - 2; i++) {
        char c = name[i];
        if (c == '/') {
            out[k++] = '_';
            out[k++] = '_';
        } else if (c == '-') {
            out[k++] = '_';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') {
            out[k++] = c;
        } else {
            out[k++] = '_';
        }
    }
    out[k] = '\0';
}

/* Emit a C header file for a module. Contains declarations (not definitions).
 * When separate_compilation is true (Phase M3): only exported functions are
 * declared, and #includes for each imported module's header are emitted. */
int emit_header(Buf *out, const char *module_name, const Expr *program, bool separate_compilation) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit_header: expected EX_PROGRAM\n");
        return -1;
    }

    char guard[256];
    sanitize_module_name(guard, module_name, sizeof(guard));

    /* Header guard */
    buf_printf(out, "/* generated by tur (phase 2) */\n");
    buf_printf(out, "#ifndef TUR_%s_H\n", guard);
    buf_printf(out, "#define TUR_%s_H\n\n", guard);

    /* Standard includes */
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdlib.h>\n");
    buf_puts(out, "#include <string.h>\n\n");

    /* Phase M3: When separate compilation, emit #includes for imported modules. */
    if (separate_compilation) {
        for (uint32_t i = 0; i < program->as.program.n; i++) {
            const Expr *e = program->as.program.items[i];
            if (e->kind == EX_DEFMODULE) {
                const DefModule *mod = e->as.defmodule_.mod;
                for (uint32_t j = 0; j < mod->n_imports; j++) {
                    char imp_mangled[256];
                    mangle_module_name(imp_mangled, mod->imports[j].module_name->name,
                                       sizeof(imp_mangled));
                    buf_printf(out, "#include \"%s.h\"\n", imp_mangled);
                }
                if (mod->n_imports > 0) buf_putc(out, '\n');
            }
        }
    }

    /* Forward declarations for functions.
     * In separate_compilation mode, only emit exported symbols. */
    uint32_t h_n_items;
    const Expr **h_items = flatten_program_items(program, &h_n_items);
    for (uint32_t i = 0; i < h_n_items; i++) {
        const Expr *e = h_items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            const char *fn_name = raw_name_for_binding(fd->binding);
            bool is_main = (strcmp(fd->binding->name->name, "main") == 0);

            if (is_main) { free((void*)fn_name); continue; }

            /* In separate_compilation mode, only declare exported symbols. */
            if (separate_compilation && !fd->binding->is_exported) {
                free((void*)fn_name); continue;
            }

            /* Emit function declaration */
            if (e->type.kind == TY_FN) {
                TypeKind result = e->type.as.fn.result_kind;
                buf_puts(out, type_c_name(emit_type_from_kind(result)));
            } else {
                buf_puts(out, "void");
            }
            buf_printf(out, " %s(", fn_name);
            for (uint8_t j = 0; j < fd->n_params; j++) {
                if (j > 0) buf_puts(out, ", ");
                buf_puts(out, type_c_name(fd->param_types[j]));
            }
            buf_puts(out, ");\n");
            free((void*)fn_name);
        } else if (e->kind == EX_EXTERN_C) {
            ExternC *ec = e->as.extern_c_.ext;
            char *ec_mangled = mangle_field_name(ec->c_name->name);
            buf_printf(out, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec_mangled);
            free(ec_mangled);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(out, ", ");
                buf_puts(out, type_c_name(ec->param_types[j]));
            }
            buf_puts(out, ");\n");
        } else if (e->kind == EX_DEF && !e->as.def_.struct_def) {
            /* Phase M6: exported global variables need extern declarations
             * in the header so other modules can reference them. */
            if (separate_compilation && e->as.def_.binding->is_exported) {
                const char *var_name = raw_name_for_binding(e->as.def_.binding);
                buf_printf(out, "extern %s %s;\n",
                           type_c_name(e->as.def_.binding->type), var_name);
                free((void*)var_name);
            }
        }
    }
    free(h_items);

    if (out->len > 0 && out->data[out->len - 1] != '\n') {
        buf_putc(out, '\n');
    }

    /* Header guard end */
    buf_printf(out, "#endif /* TUR_%s_H */\n", guard);
    return 0;
}

/* Emit a C implementation file for a module. Contains definitions. */
/* Emit a C implementation file for a module. Contains definitions.
 * When separate_compilation is true (Phase M3): #includes imported modules'
 * headers instead of emitting their code inline. */
int emit_implementation(Buf *out, const char *module_name, const Expr *program, bool separate_compilation) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit_implementation: expected EX_PROGRAM\n");
        return -1;
    }

    Buf file; buf_init(&file);
    Buf body; buf_init(&body);

    EmitCtx ctx;
    ctx.file = &file;
    ctx.main_ = &body;
    ctx.indent = 4;
    ctx.tmp_n = 0;
    ctx.fn_params = NULL;
    ctx.n_fn_params = 0;
    /* Phase 3: closure tracking */
    ctx.closure = NULL;
    ctx.env_var_name = NULL;
    /* Phase 3: env struct tracking */
    ctx.env_struct_names = NULL;
    ctx.n_env_struct_names = 0;
    ctx.cap_env_struct_names = 0;
    /* Phase 3/4: Track return emission */
    ctx.return_emitted = false;
    /* Phase 19: Pending effect handler function buffer */
    Buf pending_hfns2; buf_init(&pending_hfns2);
    ctx.pending_handler_fns = &pending_hfns2;
    /* Phase R5: no-unwind context (false at top level; set per-function) */
    ctx.no_unwind = false;
    /* Phase M3: separate compilation mode */
    ctx.separate_compilation = separate_compilation;
    /* Phase 19D: handle captures (NULL at top level) */
    ctx.handle_captures = NULL;
    ctx.n_handle_captures = 0;
    ctx.handle_env_name = NULL;

    char guard[256];
    sanitize_module_name(guard, module_name, sizeof(guard));

    /* Include the corresponding header (which already pulls in imported headers
     * when separate_compilation is true). */
    buf_printf(out, "/* generated by tur (phase 2) */\n");
    buf_printf(out, "#include \"%s.h\"\n\n", guard);

    uint32_t impl_n_items;
    const Expr **impl_items = flatten_program_items(program, &impl_n_items);

    /* Check if user defined a main function */
    bool user_has_main = false;
    for (uint32_t i = 0; i < impl_n_items; i++) {
        const Expr *e = impl_items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            if (strcmp(fd->binding->name->name, "main") == 0) {
                user_has_main = true;
                break;
            }
        }
    }

    /* Phase M5: collect top-level EX_DEFER nodes for atexit registration. */
    uint32_t n_module_defers = 0;
    for (uint32_t i = 0; i < impl_n_items; i++) {
        if (impl_items[i]->kind == EX_DEFER) n_module_defers++;
    }
    const Expr **module_defers = NULL;
    if (n_module_defers > 0) {
        module_defers = (const Expr **)malloc(n_module_defers * sizeof(Expr *));
        uint32_t di = 0;
        for (uint32_t i = 0; i < impl_n_items; i++) {
            if (impl_items[i]->kind == EX_DEFER)
                module_defers[di++] = impl_items[i];
        }
    }

    /* Pass 1: emit all top-level definitions */
    for (uint32_t i = 0; i < impl_n_items; i++) {
        const Expr *e = impl_items[i];
        if (e->kind == EX_DEFER) {
            /* Phase M5: module-level defers are handled after this pass. */
            continue;
        } else if (e->kind == EX_DEF) {
            char *bn = name_for_binding(&ctx, e->as.def_.binding);
            /* Phase M6: exported def bindings get extern linkage in separate
             * compilation mode so other modules can reference them. */
            bool def_needs_static = !(separate_compilation &&
                                      e->as.def_.binding->is_exported);
            buf_printf(&file, "%s%s %s;\n",
                       def_needs_static ? "static " : "",
                       type_c_name(e->as.def_.binding->type), bn);
            if (e->as.def_.init) {
                char *iv = emit_value(&ctx, &body, e->as.def_.init);
                indent_buf(&body, ctx.indent);
                buf_printf(&body, "%s = %s;\n", bn, iv);
                free(iv);
            }
            free(bn);
        } else if (e->kind == EX_FN_DEF) {
            /* Emit function definition */
            emit_fn_def(&ctx, &file, e);
        } else if (e->kind == EX_EXTERN_C) {
            /* In implementation, just emit the extern declaration reference */
            ExternC *ec = e->as.extern_c_.ext;
            char *ec_mangled = mangle_field_name(ec->c_name->name);
            buf_printf(&file, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec_mangled);
            free(ec_mangled);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(&file, ", ");
                buf_puts(&file, type_c_name(ec->param_types[j]));
            }
            buf_puts(&file, ");\n");
        } else {
            emit_stmt(&ctx, &body, e);
        }
    }
    free(impl_items);

    /* Phase M5: emit module-level defer thunks + atexit constructor. */
    if (n_module_defers > 0) {
        buf_puts(&file, "\n/* Phase M5: module-level defers */\n");
        /* Emit one static thunk per deferred expression. */
        for (uint32_t i = 0; i < n_module_defers; i++) {
            buf_printf(&file, "static void __module_defer_%u(void) {\n", i);
            Buf thunk_body; buf_init(&thunk_body);
            const char *saved_frame = ctx.frame_var;
            ctx.frame_var = NULL;
            ctx.indent = 4;
            emit_stmt(&ctx, &thunk_body, module_defers[i]->as.defer_.body);
            ctx.frame_var = saved_frame;
            buf_write(&file, thunk_body.data, thunk_body.len);
            buf_free(&thunk_body);
            buf_puts(&file, "}\n");
        }
        /* Emit constructor that registers thunks via atexit in definition
         * order. atexit is LIFO so last-defined defer fires first, matching
         * function-level defer semantics. */
        buf_printf(&file,
            "__attribute__((constructor))\n"
            "static void __module_defers_%s_init(void) {\n", guard);
        for (uint32_t i = 0; i < n_module_defers; i++) {
            buf_printf(&file, "    atexit(__module_defer_%u);\n", i);
        }
        buf_puts(&file, "}\n");
        free(module_defers);
    }

    /* Assemble: includes + file-scope decls + body (initializers).
     * In separate_compilation (M3) mode, never emit an auto-generated main():
     * each module is compiled independently and the user is responsible for
     * providing exactly one explicit main() across all modules. */
    if (file.len) { buf_write(out, file.data, file.len); buf_putc(out, '\n'); }
    if (!separate_compilation && !user_has_main) {
        /* Only generate main() if user didn't define one (single-file mode) */
        buf_puts(out, "int main(int argc, char **argv) {\n");
        /* Phase R6: Set g_panic_trace from compiler flag */
        if (g_panic_trace) {
            buf_puts(out, "    g_panic_trace = 1;\n");
        }
        /* CLI-ARGS: Build *args* list from argv[1..] as a linked list of char* (as int64_t). */
        buf_puts(out, "    /* *args*: build cons list from argv[1..argc-1] */\n");
        buf_puts(out, "    g_tur_args = 0;\n");
        buf_puts(out, "    for (int _ai = argc - 1; _ai >= 1; _ai--) {\n");
        buf_puts(out, "        typedef struct { int64_t value; int64_t next; } __tur_args_cell;\n");
        buf_puts(out, "        __tur_args_cell *_c = (__tur_args_cell *)malloc(sizeof(__tur_args_cell));\n");
        buf_puts(out, "        _c->value = (int64_t)(intptr_t)argv[_ai];\n");
        buf_puts(out, "        _c->next = g_tur_args;\n");
        buf_puts(out, "        g_tur_args = (int64_t)(intptr_t)_c;\n");
        buf_puts(out, "    }\n");
        if (body.len) buf_write(out, body.data, body.len);
        buf_puts(out, "    return 0;\n");
        buf_puts(out, "}\n");
    }

    buf_free(&file);
    buf_free(&body);
    free(ctx.env_struct_names);
    return 0;
}
