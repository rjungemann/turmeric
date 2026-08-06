/* elab_module.c -- module loading, imports, exports, and symbol resolution. */
#include "elab_internal.h"

/* ---- file-local helper forward declarations ---- */
static bool module_name_valid(const char *name, uint32_t len);
static ElabModule *elab_load_module(Elab *e, const Symbol *name, Span import_span);
static bool parse_import_spec(Elab *e, const Form *f, ImportSpec *out);
static void elab_forward_declare_defns(Elab *e, Form *const *items,
                                       uint32_t start, uint32_t end);

/* Pass 1: forward-declare every top-level `defn` in items[start..end) into the
 * global scope, so mutually- and self-recursive functions in the range can see
 * each other before their bodies elaborate.  Mirrors the pre-pass the entry
 * unit runs in elaborate_program (elab_toplevel.c) -- extracted here so both
 * the defmodule body (elab_defmodule) and the top-level forms of an imported
 * module (elab_load_module, where a spliced `(load ...)` can drop a
 * self-recursive defn like typeclass-show.tur's `vec-show-loop`) get the same
 * forward declarations.  Without it, a self-recursive spliced defn's own
 * recursive call resolved to "unknown function or operator"; see
 * docs/reported/compiled-string-return-int-conversion.md (secondary blocker). */
static void elab_forward_declare_defns(Elab *e, Form *const *items,
                                       uint32_t start, uint32_t end) {
    for (uint32_t j = start; j < end; j++) {
        Form *f = items[j];
        if (f->tag != F_LIST || f->as.list.len == 0) continue;
        Form *h = f->as.list.items[0];
        if (h->tag != F_SYM || h->as.sym != e->sym_defn) continue;
        if (f->as.list.len < 3) continue;
        /* Skip optional #[no-unwind] / #[used] bare attribute symbols
         * (either order) before the name. */
        uint32_t name_idx = 1;
        while ((uint32_t)f->as.list.len > name_idx &&
               f->as.list.items[name_idx]->tag == F_SYM &&
               (f->as.list.items[name_idx]->as.sym == e->sym_no_unwind_attr ||
                f->as.list.items[name_idx]->as.sym == e->sym_used_attr)) {
            name_idx++;
        }
        if ((uint32_t)f->as.list.len <= name_idx) continue;
        Form *fn_name_f = f->as.list.items[name_idx];
        if (fn_name_f->tag != F_SYM) continue;
        /* Check not already defined */
        if (scope_lookup(&e->global, fn_name_f->as.sym)) continue;
        /* SS3a / general: scan for return-type annotation in the defn
         * form to get the real result_kind for the forward declaration.
         * Without this, recursive functions declared :nil infer TY_INT
         * (the placeholder) and emit as int64_t.
         * params vector is at name_idx+1; return type annotation is
         * at name_idx+2 if it is F_KEYWORD or F_TYPE_ANN. */
        TypeKind fwd_result_kind = TY_INT; /* placeholder */
        uint32_t ret_idx = name_idx + 2;
        /* Skip optional #{Unsafe} / effect-row annotation (F_MAP) */
        if (ret_idx < (uint32_t)f->as.list.len && f->as.list.items[ret_idx]->tag == F_MAP) {
            ret_idx++;
        }
        if (ret_idx < (uint32_t)f->as.list.len) {
            Form *ret_f = f->as.list.items[ret_idx];
            /* Accept spaced `: T` (an F_TYPE_ANN wrapping a single
             * symbol/keyword) by unwrapping to the inner form -- mirrors the
             * top-level pre-pass in elab_toplevel.c.  Without this, a
             * recursive defn whose return type is written `: ptr<void>` (or
             * any spaced scalar) falls through to the TY_INT placeholder, so
             * the recursive call site is typed `int` and the if-branch
             * unifier rejects the body.  See
             * docs/reported/recursion-return-type-widens-to-int-inside-defmodule.md */
            if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
                (ret_f->as.list.items[0]->tag == F_SYM ||
                 ret_f->as.list.items[0]->tag == F_KEYWORD)) {
                ret_f = ret_f->as.list.items[0];
            }
            if (ret_f->tag == F_KEYWORD || ret_f->tag == F_SYM) {
                const char *kn = ret_f->as.sym->name;
                if (strcmp(kn, "int") == 0) fwd_result_kind = TY_INT;
                else if (strcmp(kn, "bool") == 0) fwd_result_kind = TY_BOOL;
                else if (strcmp(kn, "float") == 0) fwd_result_kind = TY_FLOAT;
                else if (strcmp(kn, "cstr") == 0) fwd_result_kind = TY_CSTR;
                else if (strcmp(kn, "nil") == 0
                      || strcmp(kn, "void") == 0) fwd_result_kind = TY_NIL;
                else if (strcmp(kn, "ptr") == 0
                      || strcmp(kn, "ptr<void>") == 0) fwd_result_kind = TY_PTR_VOID;
            } else if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len > 0) {
                /* Compound return type: peek at the head symbol */
                Form *head_f = ret_f->as.list.items[0];
                if (head_f->tag == F_SYM &&
                        strcmp(head_f->as.sym->name, "Session") == 0) {
                    fwd_result_kind = TY_SESSION;
                }
                /* Other compound types keep TY_INT placeholder */
            }
        }
        /* Count actual arity + scalar arg kinds from the params vector.
         * fwd_decl_scan_params skips `^`-prefixed markers (^fat/^mut/...) so
         * the arity is not over-stated -- counting `^fat` as a slot made a
         * sibling forward-reference call look under-saturated and synthesised
         * a bogus extra-arg PAP wrapper.  See
         * docs/reported/pap-defmodule-fat-fn-too-many-args.md */
        TypeKind *arg_kinds = NULL;
        uint32_t param_arity = (name_idx + 1 < (uint32_t)f->as.list.len)
            ? fwd_decl_scan_params(e->arena, f->as.list.items[name_idx + 1], &arg_kinds)
            : 0;
        Type fn_type = type_fn(arg_kinds, param_arity, fwd_result_kind);
        Binding *b = binding_new(e, fn_name_f->as.sym, fn_type, false, true, f->span);
        scope_add(&e->global, b);
    }
}

/* Phase M0: Module system */

/* Validate that a module name only contains [a-zA-Z0-9_\-/] */
static bool module_name_valid(const char *name, uint32_t len) {
    if (len == 0) return false;
    for (uint32_t i = 0; i < len; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/') {
            continue;
        }
        return false;
    }
    /* Must not start or end with '/' */
    if (name[0] == '/' || name[len - 1] == '/') return false;
    return true;
}

/* Phase M: (load "path") — source-file inclusion form.
 * Reads the file at the given path, elaborates all its top-level forms into
 * the current module's scope, and returns nil.  Uses the loaded_modules
 * registry (keyed by interned path) to prevent duplicate loads.
 *
 * Syntax: (load "relative/or/absolute/path.tur")
 */
/* Phase M: (load "path") — handled by the load-expansion preprocessor in
 * elaborate_program before the two-pass elab.  The preprocessor expands loads
 * at the compilation-unit top level AND descends into defmodule bodies (so a
 * load inside a module splices the loaded file's forms into the module scope;
 * see docs/archive/history/load-inside-defmodule-silently-loses-names.md).  Any
 * (load ...) that still reaches this point is in genuine expression position
 * (a defn/let/do body), where it cannot act as a compile-time include; emit a
 * hard error so the programmer knows to move it to the top level or a
 * defmodule body. */
Expr *elab_load(Elab *e, const Form *call) {
    diag_emit(DIAG_ERROR, call->span,
              "load is only valid at the top level or directly in a defmodule body; "
              "move it out of the enclosing defn/let body");
    return NULL;
}

/* Phase M2: Load and elaborate an imported module file.
 * Returns the registry entry (may have 0 exports on parse/elab failure).
 * Returns NULL only on fatal error (circular import or OOM). */
static ElabModule *elab_load_module(Elab *e, const Symbol *name, Span import_span) {
    /* SB2: Reject imports in sandboxed environments. */
    if (e->sandboxed) {
        diag_emit(DIAG_ERROR, import_span,
                  "import not allowed in sandboxed environment");
        return NULL;
    }

    /* Already in registry? */
    ElabModule *existing = elab_find_loaded_module(e, name);
    if (existing) {
        if (existing->is_loading) {
            diag_emit(DIAG_ERROR, import_span,
                      "circular import: module '%s' is already being loaded", name->name);
            return NULL;
        }
        return existing;
    }

    /* Reserve a registry slot first (for circular-import detection). */
    if (e->n_loaded_modules >= e->cap_loaded_modules) {
        e->cap_loaded_modules = e->cap_loaded_modules ? e->cap_loaded_modules * 2 : 8;
        e->loaded_modules = (ElabModule *)realloc(e->loaded_modules,
                             e->cap_loaded_modules * sizeof(ElabModule));
        if (!e->loaded_modules) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    uint32_t slot_idx = e->n_loaded_modules++;
    ElabModule *slot = &e->loaded_modules[slot_idx];
    slot->name = name;
    slot->exports = NULL;
    slot->n_exports = 0;
    slot->exported_macros = NULL;
    slot->n_exported_macros = 0;
    slot->exported_effects = NULL;
    slot->n_exported_effects = 0;
    slot->is_loading = true;

    /* Build file path: 'geom/vector' -> '{base_dir}/geom/vector.tur'
     * The '/' in module names maps directly to directory separators. */
    char path_buf[4096];
    const char *base = e->module_base_dir ? e->module_base_dir : ".";
    int plen = snprintf(path_buf, sizeof(path_buf), "%s/%s.tur", base, name->name);
    if (plen < 0 || (size_t)plen >= sizeof(path_buf)) {
        diag_emit(DIAG_ERROR, import_span,
                  "module path too long for '%s'", name->name);
        slot->is_loading = false;
        return NULL;
    }

    /* Read source file. */
    char *src_raw = NULL;
    size_t src_len = 0;
    if (elab_read_file(path_buf, &src_raw, &src_len) != 0) {
        /* SC0: collect every path we attempt so a final failure can list
         * the full search. `attempted` grows via snprintf; we leave headroom
         * for one more append and the trailing NUL so overflow truncates
         * cleanly rather than producing a torn message. */
        char attempted[8192];
        int alen = 0;
        int w = snprintf(attempted + alen, sizeof(attempted) - (size_t)alen,
                         "    %s    (importing file's directory)\n", path_buf);
        if (w > 0 && (size_t)w < sizeof(attempted) - (size_t)alen) alen += w;

        /* Fallback: try the stdlib directory (e.g. for `import turi/eval`). */
        bool found_in_stdlib = false;
        if (e->module_stdlib_dir) {
            char stdlib_path[4096];
            int splen = snprintf(stdlib_path, sizeof(stdlib_path), "%s/%s.tur",
                                 e->module_stdlib_dir, name->name);
            if (splen > 0 && (size_t)splen < sizeof(stdlib_path)) {
                if (elab_read_file(stdlib_path, &src_raw, &src_len) == 0) {
                    memcpy(path_buf, stdlib_path, (size_t)splen + 1);
                    plen = splen;
                    found_in_stdlib = true;
                } else if (alen < (int)sizeof(attempted) - 256) {
                    int sw = snprintf(attempted + alen, sizeof(attempted) - (size_t)alen,
                                      "    %s    (stdlib)\n", stdlib_path);
                    if (sw > 0 && (size_t)sw < sizeof(attempted) - (size_t)alen) alen += sw;
                }
            }
        }
        if (!found_in_stdlib) {
            /* Fallback: try each -I include directory (spice paths). */
            bool found_in_includes = false;
            int  matched_ii = -1;
            for (int ii = 0; ii < e->n_module_include_dirs && !found_in_includes; ii++) {
                char inc_path[4096];
                int iplen = snprintf(inc_path, sizeof(inc_path), "%s/%s.tur",
                                     e->module_include_dirs[ii], name->name);
                if (iplen > 0 && (size_t)iplen < sizeof(inc_path)) {
                    if (elab_read_file(inc_path, &src_raw, &src_len) == 0) {
                        memcpy(path_buf, inc_path, (size_t)iplen + 1);
                        plen = iplen;
                        found_in_includes = true;
                        matched_ii = ii;
                    } else if (alen < (int)sizeof(attempted) - 256) {
                        int iw = snprintf(attempted + alen, sizeof(attempted) - (size_t)alen,
                                          "    %s    (-I %s)\n", inc_path,
                                          e->module_include_dirs[ii]);
                        if (iw > 0 && (size_t)iw < sizeof(attempted) - (size_t)alen) alen += iw;
                    }
                }
            }
            if (found_in_includes && matched_ii >= 0
                && e->module_include_workspace_producer
                && e->module_include_workspace_producer[matched_ii]) {
                /* LS2 (local-spice-dev-workflow-plan): the import was
                 * satisfied by a workspace-sibling member's src/.  If the
                 * consumer doesn't declare that producer in its own
                 * :spices, emit a one-time warning so drift between
                 * imports and declared deps surfaces before release.
                 *
                 * Match producer by the basename of the sibling's
                 * member-relative dir (e.g. "spices/alpha" -> "alpha"),
                 * which matches the convention that :spices map keys
                 * use the producer's spice name. */
                const char *producer_path =
                    e->module_include_workspace_producer[matched_ii];
                const char *producer_basename = strrchr(producer_path, '/');
                producer_basename = producer_basename
                                    ? producer_basename + 1
                                    : producer_path;
                bool declared = false;
                for (int dj = 0; dj < e->n_module_consumer_declared_spices; dj++) {
                    if (strcmp(e->module_consumer_declared_spices[dj],
                               producer_basename) == 0) {
                        declared = true;
                        break;
                    }
                }
                if (!declared
                    && e->module_include_warned
                    && !e->module_include_warned[matched_ii]) {
                    e->module_include_warned[matched_ii] = true;
                    diag_emit(DIAG_WARNING, import_span,
                              "import '%s' resolved via workspace sibling "
                              "'%s'; declare it in :spices for release builds. "
                              "(set TUR_DEBUG_RESOLVER=1 for full resolver tracing)",
                              name->name, producer_path);
                }
            }
            if (!found_in_includes) {
                /* SC0: list every searched path and suggest a workaround.
                 * When no -I paths were passed, this is almost always an
                 * intra-spice import that needs the spice's src/ on the
                 * search path -- point at that explicitly. */
                const char *hint;
                if (e->n_module_include_dirs > 0) {
                    hint = "  hint: check the -I paths you passed, "
                           "or run `tur build <spice-src-dir>` to compile the whole spice";
                } else {
                    hint = "  hint: this looks like an intra-spice import.\n"
                           "        try `tur check -I src <file>` from the spice root,\n"
                           "        or build the whole spice with `tur build src/`";
                }
                diag_emit(DIAG_ERROR, import_span,
                          "module '%s' not found\n  searched:\n%s%s",
                          name->name, attempted, hint);
                slot->is_loading = false;
                return NULL;
            }
        }
    }

    /* Copy source into the arena so it outlives the load. */
    char *src_copy = (char *)arena_alloc(e->arena, src_len + 1);
    memcpy(src_copy, src_raw, src_len);
    src_copy[src_len] = '\0';
    free(src_raw);

    /* Register the source file for diagnostics.  Path must also live in arena. */
    char *path_copy = (char *)arena_alloc(e->arena, (size_t)plen + 1);
    memcpy(path_copy, path_buf, (size_t)plen + 1);

    SourceFile *sfile = (SourceFile *)arena_alloc(e->arena, sizeof(SourceFile));
    *sfile = (SourceFile){0};
    sfile->path = path_copy;
    sfile->src = src_copy;
    sfile->len = src_len;
    sfile->file_id = e->next_import_file_id++;
    sfile->reader_type = READER_TURMERIC;
    diag_register_file(sfile);

    /* Parse the source into forms.
     *
     * Transitive-RM (T1): pass the shared `user_macros` registry so the
     * imported file sees the same user macros the entry file did. The
     * registry can be NULL (legacy callers / no manifest) -- in that
     * case read_all_with_registry behaves identically to read_all.
     *
     * Loading semantics: see docs/reader-macros-plan.md ("Loading
     * semantics"). `(import ...)` and `(load ...)` (elab_toplevel.c)
     * both populate this same registry; macros declared in an imported
     * module become visible to anything loaded *after* it in the
     * compile's read order. */
    uint32_t nforms = 0;
    bool had_error_before = diag_had_error();
    Form **forms = read_all_with_registry(e->arena, e->st, sfile,
                                          e->user_macros, &nforms);
    if (forms) {
        /* load-not-expanded-in-imported-or-project-modules: expand this
         * module's top-level (load "path") forms before elaboration, exactly as
         * the entry unit does. Without this a `(load ...)` at column 1 of an
         * imported file survives to elab_load and errors "load is only valid at
         * the top level". The visited set is shared with the entry, so a path
         * loaded by both is spliced once. */
        Form **expanded = NULL;
        uint32_t n_expanded = 0;
        int lrc = elab_expand_module_loads(e, e->arena, e->st, forms, nforms,
                                           &expanded, &n_expanded);
        if (lrc != 0) {
            slot->is_loading = false;
            if (!had_error_before && diag_had_error()) {
                diag_emit(DIAG_NOTE, import_span,
                          "while loading module '%s'", name->name);
            }
            return NULL;
        }
        forms = expanded;
        nforms = n_expanded;
    }
    if (!forms) {
        slot->is_loading = false;
        /* Transitive-RM (T1) decision #4: if the sub-read introduced a
         * new error, attach a `while loading module X` note at the
         * import site so the user can trace the breadcrumb back from
         * (say) an "unexpected character '#'" inside `lib/syntax.tur`
         * to the `(import lib/syntax)` that triggered the load. */
        if (!had_error_before && diag_had_error()) {
            diag_emit(DIAG_NOTE, import_span,
                      "while loading module '%s'", name->name);
        }
        return NULL;
    }

    /* Elaborate the imported module's forms.
     * Save and restore fields that track the current defmodule context so the
     * outer caller's state is not corrupted. */
    bool         saved_has_defmodule   = e->has_defmodule;
    const Symbol *saved_module_name    = e->current_module_name;
    const DefModule *saved_module      = e->current_module;
    e->has_defmodule       = false;
    e->current_module_name = NULL;
    e->current_module      = NULL;
    /* load-not-expanded-in-imported-or-project-modules: mark that the forms
     * below belong to an imported module, so self-registering forms
     * (defclass/definstance/method defs) do not register themselves for
     * emission in the importer's TU under separate compilation. */
    bool saved_in_imported_module = e->in_imported_module;
    e->in_imported_module = true;

    /* Phase M4: capture the DefModule so we can check its export list for macros. */
    const DefModule *loaded_defmod = NULL;

    /* Pass 1: forward-declare the imported module's *top-level* defns before
     * elaborating any body.  A `(load ...)` spliced into this file (e.g.
     * `(load "stdlib/string.tur")`, which transitively splices
     * typeclass-show.tur) can drop a self- or mutually-recursive top-level
     * defn like `vec-show-loop`; without this pre-pass its own recursive call
     * resolves to "unknown function or operator".  The defmodule form itself is
     * elaborated with its own inner Pass 1 (elab_defmodule), so this only
     * matters for the bare top-level defns that live alongside it.  See
     * docs/archive/compiled-string-return-int-conversion.md (secondary
     * blocker). */
    elab_forward_declare_defns(e, forms, 0, nforms);

    for (uint32_t i = 0; i < nforms; i++) {
        Expr *ex = elab_form(e, forms[i]);
        if (!ex) {
            e->has_defmodule       = saved_has_defmodule;
            e->current_module_name = saved_module_name;
            e->current_module      = saved_module;
            e->in_imported_module  = saved_in_imported_module;
            slot = &e->loaded_modules[slot_idx];
            slot->is_loading = false;
            return NULL;
        }
        /* Register EX_DEFMODULE into file_scope_defs so its body gets emitted.
         * emit.c's flatten_program_items expands these into top-level C items.
         * Phase M3: Skip inlining when compiling each module separately; the
         * implementation file #includes the imported module's header instead. */
        if (ex->kind == EX_DEFMODULE) {
            loaded_defmod = ex->as.defmodule_.mod; /* Phase M4 */
            if (!e->separate_compilation) elab_register_file_def(e, ex);
        } else if (ex->kind != EX_NIL_LIT && !e->separate_compilation) {
            /* load-not-expanded-in-imported-or-project-modules: a top-level
             * (load ...) spliced bare definitions (e.g. arrow.tur's `>>>`)
             * ahead of this file's defmodule. The entry path emits every such
             * returned expr via items[]; mirror that here by registering it for
             * file-scope emission, otherwise the spliced defns elaborate into
             * scope but never reach codegen -> link errors. Self-registering
             * forms (defclass/definstance/nested defns) already returned nil and
             * are skipped by the EX_NIL guard. */
            elab_register_file_def(e, ex);
        }
    }

    e->has_defmodule       = saved_has_defmodule;
    e->current_module_name = saved_module_name;
    e->current_module      = saved_module;
    e->in_imported_module  = saved_in_imported_module;

    /* Recursive imports during elab_form() can grow e->loaded_modules and
     * invalidate the earlier `slot` pointer. Re-acquire the reserved slot
     * by index before writing the collected exports back into it. */
    slot = &e->loaded_modules[slot_idx];

    /* Collect exported bindings: those in the global scope owned by this module. */
    uint32_t n_exp = 0;
    for (uint32_t i = 0; i < e->global.n; i++) {
        Binding *b = e->global.bindings[i];
        if (b->defining_module_name == name && b->is_exported) n_exp++;
    }
    Binding **exp_arr = (n_exp == 0) ? NULL :
        (Binding **)arena_alloc(e->arena, n_exp * sizeof(Binding *));
    uint32_t idx = 0;
    for (uint32_t i = 0; i < e->global.n; i++) {
        Binding *b = e->global.bindings[i];
        if (b->defining_module_name == name && b->is_exported)
            exp_arr[idx++] = b;
    }

    slot->exports   = exp_arr;
    slot->n_exports = n_exp;

    /* Phase M4: Collect exported macros from this module.
     * A macro is exported if its defining_module_name matches this module's name
     * AND its name appears in the module's (export ...) list. */
    slot->exported_macros   = NULL;
    slot->n_exported_macros = 0;
    if (loaded_defmod != NULL && loaded_defmod->n_exports > 0) {
        uint32_t n_mexp = 0;
        for (uint32_t i = 0; i < e->n_macros; i++) {
            MacroDef *m = e->macros[i];
            if (m->defining_module_name != name) continue;
            /* Check if this macro's name is in the export list */
            for (uint32_t j = 0; j < loaded_defmod->n_exports; j++) {
                if (loaded_defmod->exports[j] == m->name) { n_mexp++; break; }
            }
        }
        if (n_mexp > 0) {
            struct MacroDef **mexp_arr =
                (struct MacroDef **)arena_alloc(e->arena, n_mexp * sizeof(struct MacroDef *));
            uint32_t midx = 0;
            for (uint32_t i = 0; i < e->n_macros; i++) {
                MacroDef *m = e->macros[i];
                if (m->defining_module_name != name) continue;
                for (uint32_t j = 0; j < loaded_defmod->n_exports; j++) {
                    if (loaded_defmod->exports[j] == m->name) {
                        mexp_arr[midx++] = m; break;
                    }
                }
            }
            slot->exported_macros   = mexp_arr;
            slot->n_exported_macros = n_mexp;
        }
    }

    /* PR5-3-D: Collect exported effects for this module. */
    {
        uint32_t n_eeff = 0;
        for (uint32_t i = 0; i < e->effect_env->n_effects; i++) {
            Effect *eff = e->effect_env->effects[i];
            if (eff->defining_module_name == name && eff->is_exported) n_eeff++;
        }
        Effect **eeff_arr = (n_eeff == 0) ? NULL :
            (Effect **)arena_alloc(e->arena, n_eeff * sizeof(Effect *));
        uint32_t eidx = 0;
        for (uint32_t i = 0; i < e->effect_env->n_effects; i++) {
            Effect *eff = e->effect_env->effects[i];
            if (eff->defining_module_name == name && eff->is_exported)
                eeff_arr[eidx++] = eff;
        }
        slot->exported_effects   = eeff_arr;
        slot->n_exported_effects = n_eeff;
    }

    slot->is_loading = false;
    return slot;
}

/* Parse a single (import module-name [:as alias] [:refer [syms...]]) form */
static bool parse_import_spec(Elab *e, const Form *f, ImportSpec *out) {
    if (f->tag != F_LIST || f->as.list.len < 2) {
        diag_emit(DIAG_ERROR, f->span,
                  "import requires a module name: (import module-name [:as alias] [:refer [syms...]])");
        return false;
    }
    Form *head = f->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e->sym_import) {
        diag_emit(DIAG_ERROR, f->span, "expected (import ...)");
        return false;
    }
    Form *name_f = f->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "import module name must be a symbol");
        return false;
    }
    if (!module_name_valid(name_f->as.sym->name, name_f->as.sym->len)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "invalid module name '%s': only alphanumeric, '-', '_', '/' allowed",
                  name_f->as.sym->name);
        return false;
    }
    out->module_name = name_f->as.sym;
    out->alias = NULL;
    out->refer_syms = NULL;
    out->n_refer = 0;
    out->refer_effect_syms = NULL;
    out->n_refer_effects   = 0;
    out->span = f->span;

    uint32_t i = 2;
    while (i < f->as.list.len) {
        Form *kw = f->as.list.items[i];
        if (kw->tag == F_KEYWORD && kw->as.sym == e->kw_as) {
            i++;
            if (i >= f->as.list.len) {
                diag_emit(DIAG_ERROR, kw->span, ":as requires an alias symbol");
                return false;
            }
            Form *alias_f = f->as.list.items[i];
            if (alias_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, alias_f->span, ":as alias must be a symbol");
                return false;
            }
            out->alias = alias_f->as.sym;
            i++;
        } else if (kw->tag == F_KEYWORD && kw->as.sym == e->kw_refer) {
            i++;
            if (i >= f->as.list.len) {
                diag_emit(DIAG_ERROR, kw->span, ":refer requires a vector of symbols");
                return false;
            }
            Form *refer_f = f->as.list.items[i];
            if (refer_f->tag != F_VEC) {
                diag_emit(DIAG_ERROR, refer_f->span, ":refer requires a vector [sym1 sym2 ...]");
                return false;
            }
            uint32_t n = refer_f->as.list.len;
            /* Count plain symbols vs (effect Name) entries. */
            uint32_t n_syms = 0, n_effs = 0;
            for (uint32_t j = 0; j < n; j++) {
                Form *sf = refer_f->as.list.items[j];
                if (sf->tag == F_SYM) {
                    n_syms++;
                } else if (sf->tag == F_LIST && sf->as.list.len == 2
                           && sf->as.list.items[0]->tag == F_SYM
                           && sf->as.list.items[0]->as.sym == e->sym_effect) {
                    n_effs++;
                } else {
                    diag_emit(DIAG_ERROR, sf->span,
                              ":refer list must contain symbols or (effect Name) forms");
                    return false;
                }
            }
            const Symbol **syms = (n_syms == 0) ? NULL :
                (const Symbol **)arena_alloc(e->arena, n_syms * sizeof(Symbol *));
            const Symbol **esyms = (n_effs == 0) ? NULL :
                (const Symbol **)arena_alloc(e->arena, n_effs * sizeof(Symbol *));
            uint32_t si = 0, ei = 0;
            for (uint32_t j = 0; j < n; j++) {
                Form *sf = refer_f->as.list.items[j];
                if (sf->tag == F_SYM) {
                    syms[si++] = sf->as.sym;
                } else {
                    /* (effect Name) -- already validated above */
                    Form *en = sf->as.list.items[1];
                    if (en->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, en->span, "effect name in :refer must be a symbol");
                        return false;
                    }
                    esyms[ei++] = en->as.sym;
                }
            }
            out->refer_syms        = syms;
            out->n_refer           = n_syms;
            out->refer_effect_syms = esyms;
            out->n_refer_effects   = n_effs;
            i++;
        } else {
            diag_emit(DIAG_ERROR, kw->span,
                      "unexpected token in import; expected :as or :refer");
            return false;
        }
    }
    return true;
}

Expr *elab_defmodule(Elab *e, const Form *call) {
    /* Only valid at the top level */
    if (e->scope != &e->global) {
        diag_emit(DIAG_ERROR, call->span, "defmodule is only valid at the top level");
        return NULL;
    }
    /* Syntax: (defmodule name [docstring] (export ...) (import ...)... body...) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmodule requires a module name: (defmodule name ...)");
        return NULL;
    }
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defmodule name must be a symbol");
        return NULL;
    }
    if (!module_name_valid(name_f->as.sym->name, name_f->as.sym->len)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "invalid module name '%s': only alphanumeric, '-', '_', '/' allowed",
                  name_f->as.sym->name);
        return NULL;
    }
    if (e->has_defmodule) {
        diag_emit(DIAG_ERROR, call->span, "only one defmodule is allowed per file");
        return NULL;
    }
    e->has_defmodule = true;

    uint32_t i = 2;
    const char *docstring = NULL;

    /* Optional docstring */
    if (i < call->as.list.len && call->as.list.items[i]->tag == F_STR) {
        docstring = call->as.list.items[i]->as.s.p;
        i++;
    }

    /* Collect export symbols */
    const Symbol **exports = NULL;
    uint32_t n_exports = 0;
    uint32_t cap_exports = 0;
    const Symbol **exp_effects = NULL;
    uint32_t n_exp_effects = 0;
    uint32_t cap_exp_effects = 0;
    /* G3: names from `(export (mut g))` -- exported AND writable from outside. */
    const Symbol **exp_mut = NULL;
    uint32_t n_exp_mut = 0;
    uint32_t cap_exp_mut = 0;

    /* Collect import specs */
    ImportSpec *imports = NULL;
    uint32_t n_imports = 0;
    uint32_t cap_imports = 0;

    /* Consume (export ...) and (import ...) forms */
    while (i < call->as.list.len) {
        Form *item = call->as.list.items[i];
        if (item->tag != F_LIST || item->as.list.len == 0) break;
        Form *head = item->as.list.items[0];
        if (head->tag != F_SYM) break;

        if (head->as.sym == e->sym_export) {
            /* Parse (export sym1 sym2 ... (effect Name) ... (mut g) ...) */
            for (uint32_t j = 1; j < item->as.list.len; j++) {
                Form *sf = item->as.list.items[j];
                if (sf->tag == F_SYM) {
                    /* Regular symbol export */
                    if (n_exports >= cap_exports) {
                        cap_exports = cap_exports ? cap_exports * 2 : 4;
                        exports = (const Symbol **)realloc(exports, cap_exports * sizeof(Symbol *));
                    }
                    exports[n_exports++] = sf->as.sym;
                } else if (sf->tag == F_LIST && sf->as.list.len == 2
                           && sf->as.list.items[0]->tag == F_SYM
                           && sf->as.list.items[0]->as.sym == e->sym_effect) {
                    /* (effect Name) export */
                    Form *ename_f = sf->as.list.items[1];
                    if (ename_f->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, ename_f->span,
                                  "effect name in export list must be a symbol");
                        free(exports); free(exp_effects); free(exp_mut); free(imports);
                        return NULL;
                    }
                    if (n_exp_effects >= cap_exp_effects) {
                        cap_exp_effects = cap_exp_effects ? cap_exp_effects * 2 : 4;
                        exp_effects = (const Symbol **)realloc(exp_effects, cap_exp_effects * sizeof(Symbol *));
                    }
                    exp_effects[n_exp_effects++] = ename_f->as.sym;
                } else if (sf->tag == F_LIST && sf->as.list.len == 2
                           && sf->as.list.items[0]->tag == F_SYM
                           && sf->as.list.items[0]->as.sym == e->sym_export_mut) {
                    /* G3: (mut g) -- export g AND permit writes from outside.
                     * The name is exported normally as well, so a reader needs
                     * no second entry. */
                    Form *gname_f = sf->as.list.items[1];
                    if (gname_f->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, gname_f->span,
                                  "name in (mut ...) export must be a symbol");
                        free(exports); free(exp_effects); free(exp_mut); free(imports);
                        return NULL;
                    }
                    if (n_exports >= cap_exports) {
                        cap_exports = cap_exports ? cap_exports * 2 : 4;
                        exports = (const Symbol **)realloc(exports, cap_exports * sizeof(Symbol *));
                    }
                    exports[n_exports++] = gname_f->as.sym;
                    if (n_exp_mut >= cap_exp_mut) {
                        cap_exp_mut = cap_exp_mut ? cap_exp_mut * 2 : 4;
                        exp_mut = (const Symbol **)realloc(exp_mut, cap_exp_mut * sizeof(Symbol *));
                    }
                    exp_mut[n_exp_mut++] = gname_f->as.sym;
                } else {
                    diag_emit(DIAG_ERROR, sf->span,
                              "export list entries must be symbols, (effect Name), "
                              "or (mut global) forms");
                    free(exports); free(exp_effects); free(exp_mut); free(imports);
                    return NULL;
                }
            }
            i++;
        } else if (head->as.sym == e->sym_import) {
            /* Parse (import module-name [:as alias] [:refer [syms...]]) */
            if (n_imports >= cap_imports) {
                cap_imports = cap_imports ? cap_imports * 2 : 4;
                imports = (ImportSpec *)realloc(imports, cap_imports * sizeof(ImportSpec));
            }
            if (!parse_import_spec(e, item, &imports[n_imports])) {
                free(exports); free(exp_effects); free(exp_mut); free(imports);
                return NULL;
            }
            n_imports++;
            i++;
        } else {
            break; /* Body starts here */
        }
    }

    uint32_t body_start = i;

    /* M1: Allocate module struct early and populate imports so elab_lookup_sym
     * can resolve qualified names and import aliases during body elaboration. */
    DefModule *mod = (DefModule *)arena_alloc(e->arena, sizeof(DefModule));
    memset(mod, 0, sizeof(DefModule));
    mod->name = name_f->as.sym;
    mod->docstring = docstring;

    /* Copy exports to arena (used for marking after pass 2) */
    if (n_exports > 0) {
        mod->exports = (const Symbol **)arena_alloc(e->arena, n_exports * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_exports; j++) mod->exports[j] = exports[j];
        mod->n_exports = n_exports;
    }
    free(exports); exports = NULL;

    /* G3: copy the writable-export list to the arena beside the exports. */
    if (n_exp_mut > 0) {
        mod->exports_mut = (const Symbol **)arena_alloc(e->arena, n_exp_mut * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_exp_mut; j++) mod->exports_mut[j] = exp_mut[j];
        mod->n_exports_mut = n_exp_mut;
    }
    free(exp_mut); exp_mut = NULL;

    /* PR5-3-B: Copy exported_effects to arena */
    if (n_exp_effects > 0) {
        mod->exported_effects = (const Symbol **)arena_alloc(e->arena, n_exp_effects * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_exp_effects; j++) mod->exported_effects[j] = exp_effects[j];
        mod->n_exported_effects = n_exp_effects;
    }
    free(exp_effects); exp_effects = NULL;

    /* Copy imports to arena (needed for alias resolution during elaboration) */
    if (n_imports > 0) {
        mod->imports = (ImportSpec *)arena_alloc(e->arena, n_imports * sizeof(ImportSpec));
        for (uint32_t j = 0; j < n_imports; j++) mod->imports[j] = imports[j];
        mod->n_imports = n_imports;
    }
    free(imports); imports = NULL;

    /* M1: Set module context — body bindings will inherit defining_module_name */
    e->current_module_name = mod->name;
    e->current_module = mod;

    /* M2: Process imports — load each referenced module and inject :refer symbols. */
    for (uint32_t j = 0; j < mod->n_imports; j++) {
        const ImportSpec *imp = &mod->imports[j];
        ElabModule *loaded = elab_load_module(e, imp->module_name, imp->span);
        if (!loaded) {
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        /* :refer — add each referred symbol (binding or macro) to the current scope. */
        for (uint32_t k = 0; k < imp->n_refer; k++) {
            const Symbol *ref_sym = imp->refer_syms[k];

            /* First check bindings. */
            Binding *ref_b = NULL;
            for (uint32_t m = 0; m < loaded->n_exports; m++) {
                if (loaded->exports[m]->name == ref_sym) {
                    ref_b = loaded->exports[m];
                    break;
                }
            }
            if (ref_b) {
                scope_add(&e->global, ref_b);
                continue;
            }

            /* Phase M4: Check exported macros. */
            MacroDef *ref_macro = NULL;
            for (uint32_t m = 0; m < loaded->n_exported_macros; m++) {
                if (loaded->exported_macros[m]->name == ref_sym) {
                    ref_macro = loaded->exported_macros[m];
                    break;
                }
            }
            if (ref_macro) {
                /* Inject an alias that is visible everywhere via is_referred,
                 * but keeps defining_module_name so private helpers of the
                 * original module remain accessible during expansion. */
                MacroDef *alias = (MacroDef *)arena_alloc(e->arena, sizeof(MacroDef));
                *alias = *ref_macro;
                alias->is_referred = true;
                /* Check for name collision with existing global macros. */
                if (elab_lookup_macro(e, ref_sym) != NULL) {
                    diag_emit(DIAG_ERROR, imp->span,
                              "macro '%s' from module '%s' conflicts with an existing macro",
                              ref_sym->name, imp->module_name->name);
                    e->current_module_name = NULL;
                    e->current_module = NULL;
                    return NULL;
                }
                elab_register_macro(e, alias);
                continue;
            }

            diag_emit(DIAG_ERROR, imp->span,
                      "symbol '%s' is not exported from module '%s'",
                      ref_sym->name, imp->module_name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }

        /* PR5-3-D: Process :refer [(effect Name)] imports. */
        for (uint32_t k = 0; k < imp->n_refer_effects; k++) {
            const Symbol *ref_eff_sym = imp->refer_effect_syms[k];
            Effect *ref_eff = NULL;
            for (uint32_t m = 0; m < loaded->n_exported_effects; m++) {
                if (loaded->exported_effects[m]->name == ref_eff_sym) {
                    ref_eff = loaded->exported_effects[m];
                    break;
                }
            }
            if (!ref_eff) {
                diag_emit(DIAG_ERROR, imp->span,
                          "effect '%s' is not exported from module '%s'",
                          ref_eff_sym->name, imp->module_name->name);
                e->current_module_name = NULL;
                e->current_module = NULL;
                return NULL;
            }
            /* Add to referred_effects so the visibility guard allows access. */
            if (e->n_referred_effects >= e->cap_referred_effects) {
                e->cap_referred_effects = e->cap_referred_effects ? e->cap_referred_effects * 2 : 4;
                e->referred_effects = (Effect **)realloc(e->referred_effects,
                                      e->cap_referred_effects * sizeof(Effect *));
            }
            e->referred_effects[e->n_referred_effects++] = ref_eff;
        }
    }

    /* Pass 1: forward-declare all defn bodies (for mutual recursion) */
    elab_forward_declare_defns(e, call->as.list.items, body_start,
                               call->as.list.len);

    /* Pass 2: elaborate body forms.
     *
     * On a per-form failure (elab_form returns NULL) record the error but KEEP
     * GOING, so every bad form in the module surfaces its own diagnostic.  This
     * mirrors the top-level driver in elaborate_program (elab_toplevel.c), which
     * sets rc=-1 and continues rather than bailing on the first NULL.  The old
     * behaviour returned NULL on the FIRST failing form, so a (defmodule ...)
     * with N independent type errors reported only the first one -- e.g. the
     * tourist swap_reject_test negative fixture (every defn is a deliberate
     * swap-rejection probe) emitted only one of its ~20 expected TUR-E0001s, so
     * a regression at any later probe would slip through unnoticed.  After the
     * loop a module that had any failure still returns NULL so the caller knows
     * elaboration failed; only successfully-elaborated forms are kept in body[]. */
    uint32_t n_body = call->as.list.len - body_start;
    Expr **body = (n_body == 0) ? NULL :
        (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
    uint32_t actual_n_body = 0;
    bool body_had_error = false;

    for (uint32_t j = body_start; j < call->as.list.len; j++) {
        Expr *be = elab_form(e, call->as.list.items[j]);
        if (!be) {
            body_had_error = true;
            continue;  /* keep going to surface more diagnostics */
        }
        body[actual_n_body++] = be;
    }

    if (body_had_error) {
        e->current_module_name = NULL;
        e->current_module = NULL;
        return NULL;
    }

    mod->body = body;
    mod->n_body = actual_n_body;

    /* M1: Mark exported bindings and validate they exist in this module.
     * Phase M4: macro names in the export list are also valid exports. */
    for (uint32_t j = 0; j < mod->n_exports; j++) {
        Binding *exp_b = scope_lookup(&e->global, mod->exports[j]);
        if (!exp_b || exp_b->defining_module_name != mod->name) {
            /* Not a binding — check if it's a macro defined in this module. */
            bool found_macro = false;
            for (uint32_t k = 0; k < e->n_macros; k++) {
                if (e->macros[k]->name == mod->exports[j] &&
                    e->macros[k]->defining_module_name == mod->name) {
                    found_macro = true;
                    break;
                }
            }
            if (!found_macro) {
                diag_emit(DIAG_ERROR, call->span,
                          "exported symbol '%s' is not defined in this module",
                          mod->exports[j]->name);
                diag_emit(DIAG_NOTE, call->span,
                          "ensure '%s' has a matching (defn ...) or (defmacro ...) inside the (defmodule %s ...) body, "
                          "and check for typos in the (export ...) list",
                          mod->exports[j]->name, mod->name->name);
                e->current_module_name = NULL;
                e->current_module = NULL;
                return NULL;
            }
            /* Macro exports don't need is_exported flag; they're collected by elab_load_module. */
        } else {
            exp_b->is_exported = true;
            /* G3: a global listed as `(mut g)` is writable from outside.
             * Rejected by name on anything that cannot be written, rather than
             * accepted and left inert -- `(export (mut some-defn))` would
             * otherwise be a silently-meaningless annotation, which is the
             * exact shape the `def` annotation audit exists to prevent. */
            for (uint32_t k = 0; k < mod->n_exports_mut; k++) {
                if (mod->exports_mut[k] != mod->exports[j]) continue;
                /* A top-level `defn` is `is_global` too, so staticness alone
                 * does not separate a data global from a function.  The fn type
                 * (or a backing FnDef) is what does. */
                if (!exp_b->is_global || exp_b->type.kind == TY_FN ||
                    exp_b->source_fn_def != NULL) {
                    diag_emit(DIAG_ERROR, call->span,
                              "(export (mut %s)): '%s' is not a mutable global -- "
                              "`(mut ...)` marks an exported global as writable "
                              "from outside this module; export a function plainly",
                              mod->exports[j]->name, mod->exports[j]->name);
                    e->current_module_name = NULL;
                    e->current_module = NULL;
                    return NULL;
                }
                if (!exp_b->is_mut) {
                    diag_emit(DIAG_ERROR, call->span,
                              "(export (mut %s)): '%s' is an immutable global -- "
                              "nothing can write it; declare it `^mut` or export "
                              "it plainly",
                              mod->exports[j]->name, mod->exports[j]->name);
                    e->current_module_name = NULL;
                    e->current_module = NULL;
                    return NULL;
                }
                exp_b->is_export_mut = true;
                break;
            }
        }
    }

    /* PR5-3-B: Validate and mark exported effects. */
    for (uint32_t j = 0; j < mod->n_exported_effects; j++) {
        const Symbol *eff_name = mod->exported_effects[j];
        Effect *eff = effect_env_lookup(e->effect_env, eff_name);
        if (!eff || eff->defining_module_name != mod->name) {
            diag_emit(DIAG_ERROR, call->span,
                      "exported effect '%s' is not defined in this module",
                      eff_name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        if (eff->is_private) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0021_PRIVATE_EFFECT,
                                "private effect '%s' cannot be exported",
                                eff_name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        eff->is_exported = true; /* explicit export — already true, but record intent */
    }

    /* Reset module context */
    e->current_module_name = NULL;
    e->current_module = NULL;

    Expr *out = expr_new(e->arena, EX_DEFMODULE, TYPE_NIL, call->span);
    out->as.defmodule_.mod = mod;
    return out;
}

/* used-attr-whole-program: force a module to be loaded (and thus emitted)
 * even though no `(import)` reaches it.  This retains a `#[used]` defn that is
 * reached only through its raw mangled C symbol -- a hand-written cross-module
 * inline-C bridge or by-address C-ABI callback -- on the single-file /
 * whole-program build path (`tur build <file>`, `tur run <file>`, `tur test`),
 * which inlines only the entry's Turmeric import closure and would otherwise
 * drop the module.  Loading is idempotent (elab_load_module dedups by name),
 * so force-loading an already-imported module is a no-op; and because no
 * `:refer` is applied, none of the module's names enter the entry's scope --
 * its module-private defns are simply registered for file-scope emission, just
 * as an ordinary `(import ...)` of the module would do.  No-op under separate
 * compilation (every project module is compiled and linked there already). */
void elab_force_load_module(Elab *e, const char *module_name) {
    if (!e || !module_name || !*module_name) return;
    if (e->separate_compilation) return;
    const Symbol *sym =
        symtab_intern(e->st, strslice(module_name, (uint32_t)strlen(module_name)));
    Span span = {0, 0, 0, 0, 0, 0};
    (void)elab_load_module(e, sym, span);
}

/* M1: Resolve a symbol with module visibility and qualified name support.
 * Returns the binding on success.
 * Returns NULL with *had_error = false: symbol not found, caller emits "unbound".
 * Returns NULL with *had_error = true: error already emitted, caller returns NULL. */
Binding *elab_lookup_sym(Elab *e, const Symbol *sym, Span span, bool *had_error) {
    *had_error = false;

    /* Direct scope lookup */
    Binding *b = scope_lookup(e->scope, sym);
    if (b) {
        /* The `tur/` namespace is implicitly imported everywhere, so a binding
         * defined in a `tur/` module is globally visible regardless of its
         * (export ...) list.  The M7 promotion sweep (elab_toplevel.c) normally
         * establishes that by rewriting every `tur/`-module binding's
         * defining_module_name to NULL -- but that sweep fires exactly once, at
         * the stdlib/user boundary.  On the interpreter's incremental
         * re-elaboration (TR2, now the default) the accumulated stdlib prefix
         * grows across evals, so the boundary -- and thus the promotion -- can
         * land after a form that already needed the binding.  The visible
         * symptom is a macro-expanded call to a module-private stdlib helper:
         * `(assert! ...)` expands to `tur-contract-check`, which is private to
         * tur/contract, and every contract/sized/existential fixture failed
         * with "symbol 'tur-contract-check' is private to module 'tur/contract'"
         * under --interpret while passing on the whole-program path.
         *
         * Honour the implicit `tur/` import directly at lookup time so
         * visibility no longer depends on the sweep having already run.  This
         * is the binding-side counterpart of the identical rule in
         * elab_lookup_macro (elab_core.c), added for the same reason. */
        if (b->defining_module_name != NULL
            && b->defining_module_name->len >= 4
            && memcmp(b->defining_module_name->name, "tur/", 4) == 0) {
            /* visible: implicitly-imported stdlib namespace */
        } else
        /* Visibility: private symbol accessed from outside its module */
        if (b->defining_module_name != NULL
            && e->current_module_name != b->defining_module_name
            && !b->is_exported) {
            diag_emit(DIAG_ERROR, span,
                      "symbol '%s' is private to module '%s'",
                      sym->name, b->defining_module_name->name);
            diag_emit(DIAG_NOTE, b->span,
                      "defined here; add '%s' to module '%s''s (export ...) list to expose it",
                      sym->name, b->defining_module_name->name);
            *had_error = true;
            return NULL;
        }
        /* F4 (cross-plan-followups): emit deprecation warning at the use
         * site.  Suppressed for self-recursive references so a deprecated
         * function does not warn on its own internal recursion.  Under
         * --Werror=deprecated the diagnostic is promoted to an error so
         * the elaborator stops compilation. */
        if (b->is_deprecated && b->name != e->current_fn_name) {
            DiagLevel sev = g_werror_deprecated ? DIAG_ERROR : DIAG_WARNING;
            if (b->deprecation_message) {
                diag_emit(sev, span,
                          "'%s' is deprecated: %s",
                          sym->name, b->deprecation_message);
            } else {
                diag_emit(sev, span,
                          "'%s' is deprecated", sym->name);
            }
            if (g_werror_deprecated) {
                *had_error = true;
                return NULL;
            }
        }
        return b;
    }

    /* Qualified name resolution — only if symbol contains '/' */
    const char *sym_str = sym->name;
    uint32_t    sym_len = sym->len;
    bool has_slash = false;
    for (uint32_t i = 0; i < sym_len; i++) {
        if (sym_str[i] == '/') { has_slash = true; break; }
    }
    if (!has_slash) return NULL;

    /* Self-qualified: current module name is a prefix */
    if (e->current_module_name != NULL) {
        const Symbol *mn = e->current_module_name;
        if (sym_len > mn->len + 1
            && sym_str[mn->len] == '/'
            && memcmp(sym_str, mn->name, mn->len) == 0) {
            const char *suffix     = sym_str + mn->len + 1;
            uint32_t    suffix_len = sym_len - mn->len - 1;
            const Symbol *ss = symtab_intern(e->st, strslice(suffix, suffix_len));
            Binding *b2 = scope_lookup(e->scope, ss);
            if (b2) return b2;
            diag_emit_with_code(DIAG_ERROR, span, TUR_E0003_UNBOUND_SYMBOL,
                                "unbound symbol '%.*s' in module '%s'",
                                (int)suffix_len, suffix, mn->name);
            *had_error = true;
            return NULL;
        }
    }

    /* M2: Cross-module qualified resolution. */
    if (e->current_module != NULL) {
        /* alias/sym — match by :as alias */
        for (uint32_t i = 0; i < e->current_module->n_imports; i++) {
            const ImportSpec *imp = &e->current_module->imports[i];
            if (!imp->alias) continue;
            const Symbol *alias = imp->alias;
            if (sym_len > alias->len + 1
                && sym_str[alias->len] == '/'
                && memcmp(sym_str, alias->name, alias->len) == 0) {
                const char *sym_part     = sym_str + alias->len + 1;
                uint32_t    sym_part_len = sym_len - alias->len - 1;
                const Symbol *sym_key = symtab_intern(e->st, strslice(sym_part, sym_part_len));
                ElabModule *loaded = elab_find_loaded_module(e, imp->module_name);
                if (!loaded) {
                    diag_emit(DIAG_ERROR, span,
                              "module '%s' (alias '%s') was not loaded",
                              imp->module_name->name, alias->name);
                    *had_error = true;
                    return NULL;
                }
                for (uint32_t m = 0; m < loaded->n_exports; m++) {
                    if (loaded->exports[m]->name == sym_key)
                        return loaded->exports[m];
                }
                /* M7: check if the symbol IS defined in that module but private. */
                Binding *priv = NULL;
                for (uint32_t k = 0; k < e->global.n; k++) {
                    Binding *gb = e->global.bindings[k];
                    if (gb->name == sym_key &&
                        gb->defining_module_name == imp->module_name) {
                        priv = gb; break;
                    }
                }
                diag_emit(DIAG_ERROR, span,
                          "symbol '%s' is not exported from module '%s'",
                          sym_key->name, imp->module_name->name);
                if (priv) {
                    diag_emit(DIAG_NOTE, priv->span,
                              "'%s' is defined here but is private; add it to module '%s''s (export ...) list",
                              sym_key->name, imp->module_name->name);
                }
                *had_error = true;
                return NULL;
            }
        }
        /* full-module-name/sym — match by full module name */
        for (uint32_t i = 0; i < e->current_module->n_imports; i++) {
            const ImportSpec *imp = &e->current_module->imports[i];
            const Symbol *mn = imp->module_name;
            if (sym_len > mn->len + 1
                && sym_str[mn->len] == '/'
                && memcmp(sym_str, mn->name, mn->len) == 0) {
                const char *sym_part     = sym_str + mn->len + 1;
                uint32_t    sym_part_len = sym_len - mn->len - 1;
                const Symbol *sym_key = symtab_intern(e->st, strslice(sym_part, sym_part_len));
                ElabModule *loaded = elab_find_loaded_module(e, mn);
                if (!loaded) {
                    diag_emit(DIAG_ERROR, span,
                              "module '%s' was not loaded", mn->name);
                    *had_error = true;
                    return NULL;
                }
                for (uint32_t m = 0; m < loaded->n_exports; m++) {
                    if (loaded->exports[m]->name == sym_key)
                        return loaded->exports[m];
                }
                /* M7: check if the symbol IS defined in that module but private. */
                Binding *priv = NULL;
                for (uint32_t k = 0; k < e->global.n; k++) {
                    Binding *gb = e->global.bindings[k];
                    if (gb->name == sym_key && gb->defining_module_name == mn) {
                        priv = gb; break;
                    }
                }
                diag_emit(DIAG_ERROR, span,
                          "symbol '%s' is not exported from module '%s'",
                          sym_key->name, mn->name);
                if (priv) {
                    diag_emit(DIAG_NOTE, priv->span,
                              "'%s' is defined here but is private; add it to module '%s''s (export ...) list",
                              sym_key->name, mn->name);
                }
                *had_error = true;
                return NULL;
            }
        }
    }

    return NULL; /* Not a recognised qualified name; caller handles "unbound" */
}
