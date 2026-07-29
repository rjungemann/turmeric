/* elab_global.c -- SS5: Multi-party global protocol types (defprotocol).
 *
 * Implements:
 *   elab_defprotocol  -- parses and registers a global protocol
 *   elab_make_protocol -- creates role endpoints (compile-time type check only)
 *   elab_send_to       -- type-checks (send-to chan role val)
 *   elab_recv_from     -- type-checks (recv-from chan role)
 *   elab_role_close    -- type-checks (close chan) for TY_ROLE endpoints
 *
 * Global protocols are compile-time only here in elab: the role endpoints
 * carry their step cursors as types and the message ops are type-checked
 * against them.  The runtime multi-party router (TurRouter / TurRole) is
 * emitted by emit_module.c.
 */
#include "elab_internal.h"
#include "symbols.h"
#include <string.h>
#include <stdlib.h>

/* Helper: build an EX_INLINE_C null-placeholder expression. */
static Expr *make_null_placeholder(Elab *e, Type result_type, const char *comment,
                                   Span span) {
    Expr *out = expr_new(e->arena, EX_INLINE_C, result_type, span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice(comment, (uint32_t)strlen(comment));
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* ---- helper: intern a C string via the symbol table ---- */
static const char *gi_intern(Elab *e, const char *s) {
    return intern_cstr(e->st, s)->name;
}

/* ---- SS8: bystander step projection ---- */

/* Skip any protocol steps in which `role_name` is neither sender nor receiver
 * (i.e., bystander steps).  This implements the projection rule for N-role
 * protocols: if the current role is not involved in a message exchange it
 * simply advances past it.
 *
 * Returns the first step at which `role_name` is the sender or receiver, or
 * NULL/GI_END if the protocol is finished for this role. */
static GlobalInteraction *skip_bystander_steps(GlobalInteraction *step,
                                                const char *role_name) {
    while (step && step->kind == GI_MSG) {
        bool is_sender   = step->msg.from && strcmp(step->msg.from, role_name) == 0;
        bool is_receiver = step->msg.to   && strcmp(step->msg.to,   role_name) == 0;
        if (is_sender || is_receiver) break;
        /* This role is a bystander -- advance past this step */
        step = step->msg.rest;
    }
    return step;
}

/* ---- well-formedness checking ---- */

/* Check that a role name is declared in the protocol's role list.
 * Returns true if found, emits TUR_E0223 and returns false otherwise. */
static bool check_role_declared(Elab *e, const char *role,
                                const char **roles, int n_roles, Span span,
                                const char *proto_name) {
    for (int i = 0; i < n_roles; i++) {
        if (roles[i] == role || strcmp(roles[i], role) == 0) return true;
    }
    diag_emit_with_code(DIAG_ERROR, span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                        "defprotocol '%s': role '%s' is not declared in the role list",
                        proto_name, role);
    return false;
}

/* Forward declaration for recursive parsing. */
static GlobalInteraction *parse_interactions(Elab *e, Form **forms, int n_forms,
                                             const char **roles, int n_roles,
                                             const char *proto_name, bool *ok);

/* Parse a single interaction form and return a GlobalInteraction node.
 * `rest_forms` is the slice of remaining interaction forms after this one
 * (used to build the linked rest pointer for GI_MSG, etc.).
 * Returns NULL on parse error (sets *ok = false). */
static GlobalInteraction *parse_one_interaction(Elab *e, Form *f,
                                                Form **rest_forms, int n_rest,
                                                const char **roles, int n_roles,
                                                const char *proto_name, bool *ok) {
    if (!f || f->tag != F_LIST || f->as.list.len < 1) {
        diag_emit_with_code(DIAG_ERROR, f ? f->span : (Span){0},
                            TUR_E0223_GLOBAL_NOT_WELLFORMED,
                            "defprotocol '%s': expected an interaction form (-> ...) or (choice ...) or (loop ...) or (continue ...)",
                            proto_name);
        *ok = false;
        return NULL;
    }

    Form *head = f->as.list.items[0];
    if (head->tag != F_SYM) {
        diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                            "defprotocol '%s': interaction head must be a symbol",
                            proto_name);
        *ok = false;
        return NULL;
    }

    const char *head_name = head->as.sym->name;

    /* (-> From To MsgType) */
    if (strcmp(head_name, "->") == 0) {
        if (f->as.list.len != 4) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': (-> From To MsgType) requires exactly 3 arguments",
                                proto_name);
            *ok = false;
            return NULL;
        }
        Form *from_f = f->as.list.items[1];
        Form *to_f   = f->as.list.items[2];
        Form *msg_f  = f->as.list.items[3];

        if (from_f->tag != F_SYM || to_f->tag != F_SYM) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': From and To in (-> From To MsgType) must be symbols",
                                proto_name);
            *ok = false;
            return NULL;
        }

        const char *from_role = gi_intern(e, from_f->as.sym->name);
        const char *to_role   = gi_intern(e, to_f->as.sym->name);

        /* Well-formedness: roles must be declared */
        if (!check_role_declared(e, from_role, roles, n_roles, f->span, proto_name)) {
            *ok = false; return NULL;
        }
        if (!check_role_declared(e, to_role, roles, n_roles, f->span, proto_name)) {
            *ok = false; return NULL;
        }

        /* Well-formedness: no self-send */
        if (from_role == to_role || strcmp(from_role, to_role) == 0) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': role '%s' cannot send a message to itself",
                                proto_name, from_role);
            *ok = false;
            return NULL;
        }

        /* Parse the message type */
        Type *msg_type = type_expr_from_form(e, msg_f, NULL, NULL, NULL, 0);
        if (!msg_type) { *ok = false; return NULL; }

        /* Parse the remaining interactions as the rest */
        GlobalInteraction *rest = parse_interactions(e, rest_forms, n_rest,
                                                     roles, n_roles, proto_name, ok);
        if (!*ok) return NULL;

        GlobalInteraction *gi = (GlobalInteraction *)arena_alloc(e->arena, sizeof(GlobalInteraction));
        memset(gi, 0, sizeof(*gi));
        gi->kind       = GI_MSG;
        gi->msg.from   = from_role;
        gi->msg.to     = to_role;
        gi->msg.msg    = msg_type;
        gi->msg.rest   = rest;
        return gi;
    }

    /* (choice From [label1 body...] [label2 body...] ...) */
    if (strcmp(head_name, "choice") == 0) {
        if (f->as.list.len < 4) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': (choice From branch1 branch2 ...) requires at least 2 branches",
                                proto_name);
            *ok = false;
            return NULL;
        }
        Form *decider_f = f->as.list.items[1];
        if (decider_f->tag != F_SYM) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': decider role in choice must be a symbol",
                                proto_name);
            *ok = false;
            return NULL;
        }

        const char *decider = gi_intern(e, decider_f->as.sym->name);
        if (!check_role_declared(e, decider, roles, n_roles, f->span, proto_name)) {
            *ok = false; return NULL;
        }

        int n_branches = (int)f->as.list.len - 2;
        if (n_branches < 2) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': choice requires at least 2 branches, got %d",
                                proto_name, n_branches);
            *ok = false;
            return NULL;
        }

        GlobalBranch *branches = (GlobalBranch *)arena_alloc(e->arena,
                                      (size_t)n_branches * sizeof(GlobalBranch));
        for (int bi = 0; bi < n_branches; bi++) {
            Form *branch_f = f->as.list.items[2 + bi];
            /* Branch must be a vector: [label body-forms...] */
            if (branch_f->tag != F_VEC && branch_f->tag != F_LIST) {
                diag_emit_with_code(DIAG_ERROR, branch_f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                    "defprotocol '%s': each branch in choice must be [label body...]",
                                    proto_name);
                *ok = false;
                return NULL;
            }
            if (branch_f->as.list.len < 1) {
                diag_emit_with_code(DIAG_ERROR, branch_f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                    "defprotocol '%s': branch is empty -- must have at least a label",
                                    proto_name);
                *ok = false;
                return NULL;
            }
            Form *label_f = branch_f->as.list.items[0];
            if (label_f->tag != F_SYM) {
                diag_emit_with_code(DIAG_ERROR, label_f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                    "defprotocol '%s': branch label must be a symbol",
                                    proto_name);
                *ok = false;
                return NULL;
            }

            const char *label = gi_intern(e, label_f->as.sym->name);
            Form **body_forms = branch_f->as.list.items + 1;
            int    n_body     = (int)branch_f->as.list.len - 1;

            GlobalInteraction *body_gi = parse_interactions(e, body_forms, n_body,
                                                            roles, n_roles, proto_name, ok);
            if (!*ok) return NULL;

            branches[bi].label = label;
            branches[bi].body  = body_gi;
        }

        /* Parse remaining interactions after the choice as the rest */
        GlobalInteraction *rest = parse_interactions(e, rest_forms, n_rest,
                                                     roles, n_roles, proto_name, ok);
        if (!*ok) return NULL;

        GlobalInteraction *gi = (GlobalInteraction *)arena_alloc(e->arena, sizeof(GlobalInteraction));
        memset(gi, 0, sizeof(*gi));
        gi->kind              = GI_CHOICE;
        gi->choice.decider    = decider;
        gi->choice.branches   = branches;
        gi->choice.n_branches = n_branches;
        gi->choice.rest       = rest;
        return gi;
    }

    /* (loop label body-forms...) */
    if (strcmp(head_name, "loop") == 0) {
        if (f->as.list.len < 3) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': (loop label body...) requires a label and at least one body form",
                                proto_name);
            *ok = false;
            return NULL;
        }
        Form *label_f = f->as.list.items[1];
        if (label_f->tag != F_SYM) {
            diag_emit_with_code(DIAG_ERROR, label_f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': loop label must be a symbol",
                                proto_name);
            *ok = false;
            return NULL;
        }

        const char *label = gi_intern(e, label_f->as.sym->name);

        Form **body_forms = f->as.list.items + 2;
        int    n_body     = (int)f->as.list.len - 2;

        /* Well-formedness: guarded recursion check.
         * The loop body must not be just (continue label) -- there must be at
         * least one GI_MSG or GI_CHOICE before the continue. */
        bool has_progress = false;
        for (int bi = 0; bi < n_body; bi++) {
            Form *bf = body_forms[bi];
            if (bf->tag == F_LIST && bf->as.list.len >= 1
                    && bf->as.list.items[0]->tag == F_SYM) {
                const char *bn = bf->as.list.items[0]->as.sym->name;
                if (strcmp(bn, "->") == 0 || strcmp(bn, "choice") == 0) {
                    has_progress = true;
                    break;
                }
            }
        }
        if (!has_progress) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': loop '%s' is not guarded -- the body must contain at least one (-> ...) or (choice ...) before any (continue %s)",
                                proto_name, label, label);
            *ok = false;
            return NULL;
        }

        GlobalInteraction *body_gi = parse_interactions(e, body_forms, n_body,
                                                        roles, n_roles, proto_name, ok);
        if (!*ok) return NULL;

        /* Parse remaining interactions after the loop as the rest */
        GlobalInteraction *rest = parse_interactions(e, rest_forms, n_rest,
                                                     roles, n_roles, proto_name, ok);
        if (!*ok) return NULL;

        GlobalInteraction *gi = (GlobalInteraction *)arena_alloc(e->arena, sizeof(GlobalInteraction));
        memset(gi, 0, sizeof(*gi));
        gi->kind       = GI_LOOP;
        gi->loop.label = label;
        gi->loop.body  = body_gi;
        gi->loop.rest  = rest;
        return gi;
    }

    /* (continue label) */
    if (strcmp(head_name, "continue") == 0) {
        if (f->as.list.len != 2 || f->as.list.items[1]->tag != F_SYM) {
            diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                                "defprotocol '%s': (continue label) requires exactly one symbol argument",
                                proto_name);
            *ok = false;
            return NULL;
        }
        const char *label = gi_intern(e, f->as.list.items[1]->as.sym->name);

        GlobalInteraction *gi = (GlobalInteraction *)arena_alloc(e->arena, sizeof(GlobalInteraction));
        memset(gi, 0, sizeof(*gi));
        gi->kind       = GI_CONTINUE;
        gi->cont.label = label;
        return gi;
    }

    diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0223_GLOBAL_NOT_WELLFORMED,
                        "defprotocol '%s': unknown interaction form '%s'; expected ->, choice, loop, or continue",
                        proto_name, head_name);
    *ok = false;
    return NULL;
}

/* Parse a sequence of interaction forms into a linked GI chain.
 * Returns a GI_END node if n_forms == 0. */
static GlobalInteraction *parse_interactions(Elab *e, Form **forms, int n_forms,
                                             const char **roles, int n_roles,
                                             const char *proto_name, bool *ok) {
    if (n_forms == 0) {
        GlobalInteraction *end = (GlobalInteraction *)arena_alloc(e->arena, sizeof(GlobalInteraction));
        memset(end, 0, sizeof(*end));
        end->kind = GI_END;
        return end;
    }

    /* Delegate to parse_one_interaction which recurses for the rest */
    return parse_one_interaction(e, forms[0], forms + 1, n_forms - 1,
                                 roles, n_roles, proto_name, ok);
}

/* ---- elab_defprotocol ---- */

/* (defprotocol Name [Role1 Role2 ...] interaction1 interaction2 ...)
 * Parses, validates, and registers a global protocol as a compile-time entity.
 * Returns EX_INLINE_C nil (no runtime representation). */
Expr *elab_defprotocol(Elab *e, const Form *call) {
    /* Minimum: (defprotocol Name [roles...] interaction...) -- at least 3 items */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defprotocol requires (defprotocol Name [Role ...] interaction...)");
        return NULL;
    }

    /* 1. Parse protocol name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "defprotocol: protocol name must be a symbol");
        return NULL;
    }
    const char *proto_name = gi_intern(e, name_f->as.sym->name);

    /* 2. Parse role list (a vector of symbols) */
    Form *roles_f = call->as.list.items[2];
    if (roles_f->tag != F_VEC && roles_f->tag != F_LIST) {
        diag_emit(DIAG_ERROR, roles_f->span,
                  "defprotocol: role list must be a vector [Role1 Role2 ...]");
        return NULL;
    }
    int n_roles = (int)roles_f->as.list.len;
    if (n_roles < 2) {
        diag_emit(DIAG_ERROR, roles_f->span,
                  "defprotocol '%s': a global protocol requires at least 2 roles",
                  proto_name);
        return NULL;
    }
    const char **roles = (const char **)arena_alloc(e->arena, (size_t)n_roles * sizeof(char *));
    for (int i = 0; i < n_roles; i++) {
        Form *rf = roles_f->as.list.items[i];
        if (rf->tag != F_SYM) {
            diag_emit(DIAG_ERROR, rf->span,
                      "defprotocol '%s': role names must be symbols", proto_name);
            return NULL;
        }
        roles[i] = gi_intern(e, rf->as.sym->name);
    }

    /* 3. Parse interaction forms */
    int n_interactions = (int)call->as.list.len - 3;
    Form **interaction_forms = call->as.list.items + 3;

    bool ok = true;
    GlobalInteraction *body = parse_interactions(e, interaction_forms, n_interactions,
                                                 roles, n_roles, proto_name, &ok);
    if (!ok) return NULL;

    /* 4. Build TY_GLOBAL type node */
    Type *global_t = (Type *)arena_alloc(e->arena, sizeof(Type));
    *global_t = type_global(proto_name, roles, n_roles, body);

    /* 5. Register the protocol */
    if (e->n_global_protocols >= e->cap_global_protocols) {
        uint32_t new_cap = e->cap_global_protocols == 0 ? 8 : e->cap_global_protocols * 2;
        GlobalProtocol *new_arr = (GlobalProtocol *)arena_alloc(e->arena,
                                      new_cap * sizeof(GlobalProtocol));
        if (e->n_global_protocols > 0) {
            memcpy(new_arr, e->global_protocols,
                   e->n_global_protocols * sizeof(GlobalProtocol));
        }
        e->global_protocols = new_arr;
        e->cap_global_protocols = new_cap;
    }
    e->global_protocols[e->n_global_protocols].name = proto_name;
    e->global_protocols[e->n_global_protocols].type = global_t;
    e->n_global_protocols++;

    /* 6. Return nil (compile-time declaration, no runtime value) */
    return e_nil(e, call->span);
}

/* ---- elab_make_protocol ---- */

/* (make-protocol G) -- creates one Role endpoint per declared role.
 * In SS5, roles are compile-time only.  We emit a placeholder tuple of NULL
 * values whose types are TY_ROLE.  The tuple is encoded as a TY_SESSION_PAIR
 * chain for 2-role protocols (the common case), or wrapped in a session_pair
 * chain for N roles.
 *
 * The result is always immediately destructured via vector-let by the caller:
 *   (let [[ra rb] (make-protocol Ping)] ...) */
Expr *elab_make_protocol(Elab *e, const Form *call) {
    if (call->as.list.len != 2 || call->as.list.items[1]->tag != F_SYM) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-protocol requires a protocol name: (make-protocol G)");
        return NULL;
    }

    const char *proto_name = call->as.list.items[1]->as.sym->name;

    /* Look up the protocol */
    Type *global_t = NULL;
    for (uint32_t gi = 0; gi < e->n_global_protocols; gi++) {
        if (e->global_protocols[gi].name == proto_name
                || strcmp(e->global_protocols[gi].name, proto_name) == 0) {
            global_t = e->global_protocols[gi].type;
            break;
        }
    }
    if (!global_t) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-protocol: unknown protocol '%s'", proto_name);
        return NULL;
    }

    int n_roles = global_t->as.global_.n_roles;
    /* Build an array of TY_ROLE type pointers (one per role) */
    Type **role_types = (Type **)arena_alloc(e->arena, (size_t)n_roles * sizeof(Type *));
    for (int i = 0; i < n_roles; i++) {
        role_types[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
        *role_types[i] = type_role(global_t, global_t->as.global_.roles[i],
                                   global_t->as.global_.body);
    }

    /* Build a nested pair chain: TY_SESSION_PAIR(role0, TY_SESSION_PAIR(role1, ...)).
     * For 2 roles: SESSION_PAIR(role0, role1).
     * For N>2: SESSION_PAIR(role0, SESSION_PAIR(role1, ... SESSION_PAIR(roleN-2, roleN-1))). */
    Type *result_type;
    if (n_roles == 2) {
        Type *pair_t = (Type *)arena_alloc(e->arena, sizeof(Type));
        *pair_t = type_session_pair(role_types[0], role_types[1]);
        result_type = pair_t;
    } else {
        /* Build from right to left */
        Type *inner = role_types[n_roles - 1];
        for (int i = n_roles - 2; i >= 0; i--) {
            Type *pair_t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *pair_t = type_session_pair(role_types[i], inner);
            inner = pair_t;
        }
        result_type = inner;
    }

    /* SS8: N-role protocols (N>=2) are fully supported. */

    /* SS8: emit a marker; actual codegen happens in elab_forms.c vector-let split */
    return make_null_placeholder(e, *result_type, "/*make-protocol*/", call->span);
}

/* ---- elab_send_to ---- */

/* (send-to chan role-name val)
 * Type-checks a send from the role owning chan to role-name.
 * Chan must be TY_ROLE whose current_step is GI_MSG with from == this role
 * and to == role-name.
 * Returns a new TY_ROLE with current_step advanced to rest. */
Expr *elab_send_to(Elab *e, const Form *call) {
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "send-to requires 3 arguments: (send-to chan role val)");
        return NULL;
    }

    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Form *role_f = call->as.list.items[2];
    if (role_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, role_f->span,
                  "send-to: role name must be a symbol");
        return NULL;
    }

    Expr *val = elab_form(e, call->as.list.items[3]);
    if (!val) return NULL;

    /* Validate chan is TY_ROLE */
    if (chan->type.kind != TY_ROLE) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "send-to requires a Role endpoint, got %s",
                            typekind_to_string(chan->type.kind));
        return NULL;
    }

    const char *this_role  = chan->type.as.role_.role_name;
    const char *dest_role  = gi_intern(e, role_f->as.sym->name);
    /* SS8: skip bystander steps (steps where this role is neither sender nor receiver) */
    GlobalInteraction *step = skip_bystander_steps(chan->type.as.role_.current_step, this_role);

    /* Validate current step is GI_MSG with matching from/to */
    if (!step || step->kind != GI_MSG) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "send-to: role '%s' cannot send at this point in the protocol "
                            "(current step is not a message send)",
                            this_role);
        return NULL;
    }
    if (step->msg.from == NULL || strcmp(step->msg.from, this_role) != 0) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "send-to: role '%s' is not the sender at this protocol step "
                            "(expected sender is '%s')",
                            this_role, step->msg.from ? step->msg.from : "?");
        return NULL;
    }
    if (step->msg.to == NULL || strcmp(step->msg.to, dest_role) != 0) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "send-to: role '%s' should send to '%s', not to '%s'",
                            this_role, step->msg.to ? step->msg.to : "?", dest_role);
        return NULL;
    }

    /* Type-check the value against the expected message type */
    Type *expected_msg = step->msg.msg;
    if (expected_msg && expected_msg->kind != TY_UNKNOWN
            && val->type.kind != TY_UNKNOWN
            && val->type.kind != expected_msg->kind) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0001_TYPE_MISMATCH,
                            "send-to: expected message type %s, got %s",
                            typekind_to_string(expected_msg->kind),
                            typekind_to_string(val->type.kind));
        return NULL;
    }

    /* Consume the chan binding (it's linear) */
    if (chan->kind == EX_VAR && chan->as.var.binding) {
        binding_mark_moved(chan->as.var.binding, call->span);
    }

    /* Build the advanced TY_ROLE type (current_step = rest) */
    Type advanced_type = type_role(chan->type.as.role_.global_type,
                                   this_role,
                                   step->msg.rest);

    /* SS7: Compute to_idx at elaboration time */
    Type *global_t = chan->type.as.role_.global_type;
    int to_idx = -1;
    for (int i = 0; i < global_t->as.global_.n_roles; i++) {
        if (global_t->as.global_.roles[i] == dest_role
                || strcmp(global_t->as.global_.roles[i], dest_role) == 0) {
            to_idx = i; break;
        }
    }
    if (to_idx < 0) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "send-to: role '%s' not found in protocol", dest_role);
        return NULL;
    }

    /* Emit: ({ tur_router_send(__TUR_VAL_0__, to_idx, (int64_t)(__TUR_VAL_1__)); (void*)__TUR_VAL_0__; })
     * Bare statement-expression, no __extension__ -- see the note in
     * elab_sessions.c's send_code.  src/turi/eval.c matches this by prefix. */
    char send_code[256];
    snprintf(send_code, sizeof(send_code),
             "({ tur_router_send(__TUR_VAL_0__, %d, (int64_t)(__TUR_VAL_1__)); (void *)__TUR_VAL_0__; })",
             to_idx);
    size_t send_code_len = strlen(send_code);
    char *code_str = (char *)arena_alloc(e->arena, send_code_len + 1);
    memcpy(code_str, send_code, send_code_len + 1);

    Expr *out = expr_new(e->arena, EX_INLINE_C, advanced_type, call->span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice(code_str, (uint32_t)send_code_len);
    ic->return_type = advanced_type;
    ic->captures = NULL; ic->n_captures = 0;
    ic->val_exprs = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
    ic->n_val_exprs = 2;
    /* __TUR_VAL_0__ = chan */
    if (chan->kind == EX_VAR && chan->as.var.binding) {
        Binding *chan_binding = chan->as.var.binding;
        Expr *chan_var = expr_new(e->arena, EX_VAR, chan_binding->type, call->span);
        chan_var->as.var.binding = chan_binding;
        ic->val_exprs[0] = chan_var;
    } else {
        ic->val_exprs[0] = chan;
    }
    /* __TUR_VAL_1__ = val */
    ic->val_exprs[1] = val;
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* ---- elab_recv_from ---- */

/* (recv-from chan role-name)
 * Type-checks a receive for the role owning chan from role-name.
 * Chan must be TY_ROLE whose current_step is GI_MSG with from == role-name
 * and to == this role.
 * Returns a TY_SESSION_RECV_PAIR [msg-type, new-Role]. */
Expr *elab_recv_from(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "recv-from requires 2 arguments: (recv-from chan role)");
        return NULL;
    }

    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Form *role_f = call->as.list.items[2];
    if (role_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, role_f->span,
                  "recv-from: role name must be a symbol");
        return NULL;
    }

    /* Validate chan is TY_ROLE */
    if (chan->type.kind != TY_ROLE) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv-from requires a Role endpoint, got %s",
                            typekind_to_string(chan->type.kind));
        return NULL;
    }

    const char *this_role  = chan->type.as.role_.role_name;
    const char *src_role   = gi_intern(e, role_f->as.sym->name);
    /* SS8: skip bystander steps (steps where this role is neither sender nor receiver) */
    GlobalInteraction *step = skip_bystander_steps(chan->type.as.role_.current_step, this_role);

    /* Validate current step is GI_MSG with matching from/to */
    if (!step || step->kind != GI_MSG) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv-from: role '%s' cannot receive at this point in the protocol "
                            "(current step is not a message send)",
                            this_role);
        return NULL;
    }
    if (step->msg.to == NULL || strcmp(step->msg.to, this_role) != 0) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv-from: role '%s' is not the receiver at this protocol step "
                            "(expected receiver is '%s')",
                            this_role, step->msg.to ? step->msg.to : "?");
        return NULL;
    }
    if (step->msg.from == NULL || strcmp(step->msg.from, src_role) != 0) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv-from: expected message from '%s', not from '%s'",
                            step->msg.from ? step->msg.from : "?", src_role);
        return NULL;
    }

    /* Consume the chan binding */
    if (chan->kind == EX_VAR && chan->as.var.binding) {
        binding_mark_moved(chan->as.var.binding, call->span);
    }

    /* Build [msg-type, advanced-Role] as a TY_SESSION_RECV_PAIR */
    Type *msg_type = step->msg.msg;
    if (!msg_type) {
        /* Fallback to NIL if no message type */
        msg_type = (Type *)arena_alloc(e->arena, sizeof(Type));
        *msg_type = TYPE_NIL;
    }

    Type *new_role = (Type *)arena_alloc(e->arena, sizeof(Type));
    *new_role = type_role(chan->type.as.role_.global_type,
                          this_role,
                          step->msg.rest);

    Type pair_type = type_session_recv_pair(msg_type, new_role);

    /* SS7: Compute from_idx at elaboration time */
    Type *global_t = chan->type.as.role_.global_type;
    int from_idx = -1;
    for (int i = 0; i < global_t->as.global_.n_roles; i++) {
        if (global_t->as.global_.roles[i] == src_role
                || strcmp(global_t->as.global_.roles[i], src_role) == 0) {
            from_idx = i; break;
        }
    }
    if (from_idx < 0) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv-from: role '%s' not found in protocol", src_role);
        return NULL;
    }

    /* Build code: "tur_router_recv(__TUR_VAL_0__, from_idx)" -- used by elab_forms.c */
    char recv_code[80];
    snprintf(recv_code, sizeof(recv_code), "tur_router_recv(__TUR_VAL_0__, %d)", from_idx);
    size_t recv_code_len = strlen(recv_code);
    char *code_str = (char *)arena_alloc(e->arena, recv_code_len + 1);
    memcpy(code_str, recv_code, recv_code_len + 1);

    Expr *out = expr_new(e->arena, EX_INLINE_C, pair_type, call->span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice(code_str, (uint32_t)recv_code_len);
    ic->return_type = pair_type;
    ic->captures = NULL; ic->n_captures = 0;
    ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
    ic->n_val_exprs = 1;
    /* __TUR_VAL_0__ = chan (so elab_forms.c can reference it for vi=1 too) */
    if (chan->kind == EX_VAR && chan->as.var.binding) {
        Binding *chan_binding = chan->as.var.binding;
        Expr *chan_var = expr_new(e->arena, EX_VAR, chan_binding->type, call->span);
        chan_var->as.var.binding = chan_binding;
        ic->val_exprs[0] = chan_var;
    } else {
        ic->val_exprs[0] = chan;
    }
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* Note: (close chan) for TY_ROLE endpoints is handled inline in
 * elab_session_close() (elab_sessions.c), which checks the type after
 * elaborating the argument and handles both TY_SESSION and TY_ROLE. */
