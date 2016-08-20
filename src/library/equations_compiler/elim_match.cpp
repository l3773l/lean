/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "util/flet.h"
#include "kernel/instantiate.h"
#include "library/trace.h"
#include "library/num.h"
#include "library/string.h"
#include "library/pp_options.h"
#include "library/generic_exception.h"
#include "library/util.h"
#include "library/locals.h"
#include "library/app_builder.h"
#include "library/tactic/tactic_state.h"
#include "library/tactic/revert_tactic.h"
#include "library/tactic/cases_tactic.h"
#include "library/tactic/intro_tactic.h"
#include "library/equations_compiler/equations.h"
#include "library/equations_compiler/util.h"

namespace lean {
#define trace_match(Code) lean_trace(name({"eqn_compiler", "elim_match"}), Code)
#define trace_match_detail(Code) lean_trace(name({"eqn_compiler", "elim_match_detail"}), Code)

struct elim_match_fn {
    environment     m_env;
    options         m_opts;
    metavar_context m_mctx;

    expr            m_ref;
    unsigned        m_depth{0};
    buffer<bool>    m_used_eqns;
    bool            m_lemmas{true};

    elim_match_fn(environment const & env, options const & opts,
                  metavar_context const & mctx):
        m_env(env), m_opts(opts), m_mctx(mctx) {}

    struct equation {
        list<pair<expr, expr>> m_renames;
        local_context          m_lctx;
        list<expr>             m_patterns;
        expr                   m_rhs;
        expr                   m_ref; /* for reporting errors */
        unsigned               m_idx;
        equation() {}
        equation(equation const & eqn, list<expr> const & new_patterns):
            m_renames(eqn.m_renames), m_lctx(eqn.m_lctx), m_patterns(new_patterns),
            m_rhs(eqn.m_rhs), m_ref(eqn.m_ref), m_idx(eqn.m_idx) {}
    };

    struct program {
        name           m_fn_name; /* for debugging purposes */
        /* Metavariable containing the context for the program */
        expr           m_goal;
        /* Number of variables that still need to be matched/processed */
        unsigned       m_nvars;
        list<equation> m_equations;
    };

    struct lemma {
        list<expr> m_vars;
        expr       m_eqn; /* equation (it might be conditional) */
        expr       m_proof;
        lemma() {}
        lemma(list<expr> const & vars, expr const & eqn, expr const & proof):
            m_vars(vars), m_eqn(eqn), m_proof(proof) {}
    };

    /** Result for the compilation procedure. */
    struct result {
        /* m_code is the expression that implements a program. */
        expr         m_code;
        /* List of equation lemmas that hold for m_code, and their proofs */
        list<lemma>  m_lemmas;
        result() {}
        result(expr const & c):m_code(c) {}
    };

    [[ noreturn ]] void throw_error(char const * msg) {
        throw_generic_exception(msg, m_ref);
    }

    [[ noreturn ]] void throw_error(sstream const & strm) {
        throw_generic_exception(strm, m_ref);
    }

    local_context get_local_context(expr const & mvar) {
        lean_assert(is_metavar(mvar));
        metavar_decl mdecl = *m_mctx.get_metavar_decl(mvar);
        return mdecl.get_context();
    }

    local_context get_local_context(program const & P) {
        return get_local_context(P.m_goal);
    }

    type_context mk_type_context(local_context const & lctx) {
        return mk_type_context_for(m_env, m_opts, m_mctx, lctx);
    }

    type_context mk_type_context(program const & P) {
        return mk_type_context(get_local_context(P));
    }

    std::function<format(expr const &)> mk_pp_ctx(local_context const & lctx) {
        options opts = m_opts.update(get_pp_beta_name(), false);
        type_context ctx = mk_type_context_for(m_env, opts, m_mctx, lctx);
        return ::lean::mk_pp_ctx(ctx);
    }

    std::function<format(expr const &)> mk_pp_ctx(program const & P) {
        return mk_pp_ctx(get_local_context(P));
    }

    format nest(format const & fmt) const {
        return ::lean::nest(get_pp_indent(m_opts), fmt);
    }

    unsigned get_eqns_arity(local_context const & lctx, expr const & eqns) {
        /* Naive way to retrieve the arity of the function being defined */
        lean_assert(is_equations(eqns));
        type_context ctx = mk_type_context(lctx);
        unpack_eqns ues(ctx, eqns);
        return ues.get_arity_of(0);
    }

    bool is_constructor(expr const & e) const {
        return static_cast<bool>(eqns_env_interface(m_env).is_constructor(e));
    }

    bool is_constructor_app(expr const & e) const {
        return is_constructor(get_app_fn(e));
    }

    bool is_inductive(expr const & e) const {
        return static_cast<bool>(eqns_env_interface(m_env).is_inductive(e));
    }

    bool is_inductive_app(expr const & e) const {
        return is_inductive(get_app_fn(e));
    }

    bool is_value(expr const & e) const {
        return to_num(e) || to_char(e) || to_string(e) || is_constructor(e);
    }

    /* Normalize until head is constructor or value */
    expr whnf_pattern(type_context & ctx, expr const & e) {
        if (is_inaccessible(e))
            return e;
        else
            return ctx.whnf_pred(e, [&](expr const & e) {
                    return !is_constructor_app(e) && !is_value(e);
                });
    }

    /* Normalize until head is constructor */
    expr whnf_constructor(type_context & ctx, expr const & e) {
        return ctx.whnf_pred(e, [&](expr const & e) {
                return !is_constructor_app(e);
            });
    }

    /* Normalize until head is an inductive datatype */
    expr whnf_inductive(type_context & ctx, expr const & e) {
        return ctx.whnf_pred(e, [&](expr const & e) {
                return !is_inductive_app(e);
            });
    }

    /* Store in args the parameters of the inductive datatype I */
    levels get_inductive_levels_and_params(type_context & ctx, expr const & I, buffer<expr> & params) {
        expr I1 = whnf_inductive(ctx, I);
        buffer<expr> args;
        expr const & Ifn = get_app_args(I1, args);
        unsigned nparams = eqns_env_interface(m_env).get_inductive_num_params(const_name(Ifn));
        lean_assert(nparams <= args.size());
        for (unsigned i = 0; i < nparams; i++)
            params.push_back(args[i]);
        return const_levels(Ifn);
    }

    optional<equation> mk_equation(local_context const & lctx, expr const & eqn, unsigned idx) {
        expr it = eqn;
        it = binding_body(it); /* consume fn header */
        if (is_no_equation(it)) return optional<equation>();
        type_context ctx = mk_type_context(lctx);
        buffer<expr> locals;
        while (is_lambda(it)) {
            expr type  = instantiate_rev(binding_domain(it), locals);
            expr local = ctx.push_local(binding_name(it), type);
            locals.push_back(local);
            it = binding_body(it);
        }
        lean_assert(is_equation(it));
        equation E;
        E.m_lctx = ctx.lctx();
        E.m_rhs  = instantiate_rev(equation_rhs(it), locals);
        /* The function being defined is not recursive. So, E.m_rhs
           must be closed even if we "consumed" the fn header in
           the beginning of the method. */
        lean_assert(closed(E.m_rhs));
        buffer<expr> patterns;
        get_app_args(equation_lhs(it), patterns);
        for (expr & p : patterns) {
            p = whnf_pattern(ctx, instantiate_rev(p, locals));
        }
        E.m_patterns = to_list(patterns);
        E.m_ref  = eqn;
        E.m_idx  = idx;
        return optional<equation>(E);
    }

    list<equation> mk_equations(local_context const & lctx, buffer<expr> const & eqns) {
        buffer<equation> R;
        unsigned idx = 0;
        for (expr const & eqn : eqns) {
            if (auto r = mk_equation(lctx, eqn, idx)) {
                R.push_back(*r);
                lean_assert(length(R[0].m_patterns) == length(r->m_patterns));
            } else {
                lean_assert(eqns.size() == 1);
                return list<equation>();
            }
            m_used_eqns.push_back(false);
            idx++;
        }
        return to_list(R);
    }

    program mk_program(local_context const & lctx, expr const & e) {
        lean_assert(is_equations(e));
        buffer<expr> eqns;
        to_equations(e, eqns);
        unsigned arity   = get_eqns_arity(lctx, e);
        program P;
        P.m_fn_name   = binding_name(eqns[0]);
        expr fn_type  = binding_domain(eqns[0]);
        P.m_goal      = m_mctx.mk_metavar_decl(lctx, fn_type);
        P.m_nvars     = arity;
        P.m_equations = mk_equations(lctx, eqns);
        return P;
    }

    format pp_equation(equation const & eqn) {
        format r;
        auto pp = mk_pp_ctx(eqn.m_lctx);
        bool first = true;
        for (expr const & p : eqn.m_patterns) {
            if (first) first = false; else r += format(" ");
            r += paren(pp(p));
        }
        r += space() + format(":=") + nest(line() + pp(eqn.m_rhs));
        return group(r);
    }

    format pp_program(program const & P) {
        format r;
        r += format("program") + space() + format(P.m_fn_name) + space() + format("#") + format(P.m_nvars);
        for (equation const & eqn : P.m_equations) {
            r += nest(line() + pp_equation(eqn));
        }
        return r;
    }

    template<typename Pred>
    bool all_next_pattern(program const & P, Pred && p) const {
        for (equation const & eqn : P.m_equations) {
            lean_assert(eqn.m_patterns);
            if (!p(head(eqn.m_patterns)))
                return false;
        }
        return true;
    }

    /* Return true iff the next pattern in all equations is a variable. */
    bool is_variable_transition(program const & P) const {
        return all_next_pattern(P, is_local);
    }

    /* Return true iff the next pattern in all equations is an inaccessible term. */
    bool is_inaccessible_transition(program const & P) const {
        return all_next_pattern(P, is_inaccessible);
    }

    /* Return true iff the next pattern in all equations is a constructor. */
    bool is_constructor_transition(program const & P) const {
        return all_next_pattern(P, [&](expr const & p) {
                return is_constructor_app(p) || is_value(p);
            });
    }

    /* Return true iff the next pattern of every equation is a constructor or variable,
       and there are at least one equation where it is a variable and another where it is a
       constructor. */
    bool is_complete_transition(program const & P) const {
        bool has_variable    = false;
        bool has_constructor = false;
        bool r = all_next_pattern(P, [&](expr const & p) {
                if (is_local(p)) {
                    has_variable = true; return true;
                } else if (is_constructor_app(p)) {
                    has_constructor = true; return true;
                } else {
                    return false;
                }
            });
        return r && has_variable && has_constructor;
    }

    /* Return true iff the next pattern of every equation is a value or variable,
       and there are at least one equation where it is a variable and another where it is a
       value. */
    bool is_value_transition(program const & P) const {
        bool has_value    = false;
        bool has_variable = false;
        bool r = all_next_pattern(P, [&](expr const & p) {
                if (is_local(p)) {
                    has_variable = true; return true;
                } else if (is_value(p)) {
                    has_value    = true; return true;
                } else {
                    return false;
                }
            });
        return r && has_value && has_variable;
    }

    /** Return true iff the next pattern of some equations is an inaccessible term, and
        others are not */
    bool some_inaccessible(program const & P) const {
        bool found_inaccessible     = false;
        bool found_not_inaccessible = false;
        for (equation const & eqn : P.m_equations) {
            lean_assert(eqn.m_patterns);
            expr const & p = head(eqn.m_patterns);
            if (is_inaccessible(p))
                found_inaccessible = true;
            else
                found_not_inaccessible = true;
        }
        return found_inaccessible && found_not_inaccessible;
    }

    /* See update_eqn_lhs */
    template<typename F>
    expr update_eqn_lhs_core(expr const & lhs, unsigned arity, F && updt) {
        buffer<expr> args;
        auto it = lhs;
        for (unsigned i = 0; i < arity; i++) {
            lean_assert(is_app(it));
            args.push_back(app_arg(it));
            it = app_fn(it);
        }
        return updt(args);
    }

    /* Auxiliary method for updating the function in the left-hand-side of the given (conditional) equation.
       The method assumes the left-hand-side is of the form:
              (f a_1 ... a_n)
       where n == arity.

       The function updt must construct the new left-hand-side.
       It take a buffer containing [a_n, ..., a_1]. */
    template<typename F>
    expr update_eqn_lhs(expr const & eqn, unsigned arity, F && updt) {
        if (is_pi(eqn)) {
            return update_binding(eqn, binding_domain(eqn), update_eqn_lhs(binding_body(eqn), arity, updt));
        } else {
            lean_assert(is_eq(eqn));
            buffer<expr> eqn_args;
            expr const & eq_fn = get_app_args(eqn, eqn_args);
            lean_assert(eqn_args.size() == 3);
            eqn_args[1] = update_eqn_lhs_core(eqn_args[1], arity, updt);
            return mk_app(eq_fn, eqn_args);
        }
    }

    /* Helper method for tracing intermediate lemmas produced during the compilation process. */
    void trace_lemmas(program const & P, char const * header, buffer<lemma> const & lemmas) {
        trace_match_detail({
                tout() << "[" << m_depth << "] " << header << " lemmas:\n";
                auto pp_fn = mk_pp_ctx(P);
                for (lemma const & L : lemmas) {
                    /* Replace function with its name. */
                    expr tmp_eqn = update_eqn_lhs(L.m_eqn, P.m_nvars,
                                                  [&](buffer<expr> const & args) {
                                                      return mk_rev_app(mk_constant(P.m_fn_name), args);
                                                  });
                    tout() << "    " << ::lean::nest(4, pp_fn(tmp_eqn)) << "\n";
                }});
    }

    result compile_no_equation(program const & P) {
        trace_match(tout() << "no equation transition\n";);
        lean_unreachable();
    }

    result compile_skip(program const & P) {
        trace_match(tout() << "skip transition\n";);
        lean_unreachable();
    }

    /* Update the equation left hand side

            (f a_1 ... a_n)

       where n == arity, with

            (new_fn x a_1 ... a_n) */
    expr update_eqn_for_variable_transition(expr const & eqn, unsigned arity, expr const & new_fn, expr const & x) {
        return update_eqn_lhs(eqn, arity, [&](buffer<expr> & args) {
                args.push_back(x);
                return mk_rev_app(new_fn, args);
            });
    }

    result compile_variable(program const & P) {
        lean_assert(is_variable_transition(P));
        trace_match(tout() << "variable transition\n";);
        program new_P;
        new_P.m_fn_name   = P.m_fn_name;
        buffer<name> new_names;
        optional<expr> new_goal = intron(m_env, m_opts, m_mctx, P.m_goal, 1, new_names);
        if (!new_goal) throw_ill_formed_eqns();
        lean_assert(new_names.size() == 1);
        new_P.m_goal      = *new_goal;
        new_P.m_nvars     = P.m_nvars - 1;
        name x_name       = new_names[0];
        expr x            = get_local_context(new_P).get_local_decl(x_name)->mk_ref();
        buffer<equation> new_eqns;
        for (equation const & eqn : P.m_equations) {
            equation new_eqn   = eqn;
            new_eqn.m_patterns = tail(eqn.m_patterns);
            new_eqn.m_renames  = cons(mk_pair(head(eqn.m_patterns), x), eqn.m_renames);
            new_eqns.push_back(new_eqn);
        }
        new_P.m_equations = to_list(new_eqns);
        result R = compile_core(new_P);
        result new_R;
        type_context ctx = mk_type_context(P);
        new_R.m_code     = m_mctx.instantiate_mvars(P.m_goal);
        if (m_lemmas) {
            buffer<lemma> new_lemmas;
            for (lemma const & L : R.m_lemmas) {
                lemma new_L;
                new_L.m_vars  = cons(x, L.m_vars);
                new_L.m_eqn   = update_eqn_for_variable_transition(L.m_eqn, new_P.m_nvars, new_R.m_code, x);
                new_L.m_proof = L.m_proof;
                new_lemmas.push_back(new_L);
            }
            trace_lemmas(P, "variable transition", new_lemmas);
            new_R.m_lemmas = to_list(new_lemmas);
        }
        return new_R;
    }

    /* Populate R with the given equations. The equations are also updated by replacing the current
       pattern (a constructor) with its arguments. Note that R[i].first is the name of the constructor.

       Example: suppose the input eqns contains the equations

             nil          L_1 := R_1
             (cons a b)   L_2 := R_2
             (cons c d)   L_3 := R_3
             nil          L_4 := R_4

       Then, R will contain the pairs

             (nil,               L_1 := R_1)
             (cons,  (cons a b)  L_2 := R_2)
             (cons,  (cons c d)  L_3 := R_3)
             (nil                L_4 := R_4)
    */
    void distribute_constructor_equations(list<equation> const & eqns, buffer<pair<name, equation>> & R) {
        for (equation const & eqn : eqns) {
            lean_assert(eqn.m_patterns);
            type_context ctx = mk_type_context(eqn.m_lctx);
            expr pattern     = whnf_constructor(ctx, head(eqn.m_patterns));
            if (!is_constructor_app(pattern)) {
                throw_error("equation compiler failed, pattern is not a constructor "
                            "(use 'set_option trace.eqn_compiler.elim_match true' for additional details)");
            }
            list<expr> new_patterns = cons(pattern, tail(eqn.m_patterns));
            expr const & C = get_app_fn(pattern);
            R.emplace_back(const_name(C), equation(eqn, new_patterns));
        }
    }

    /* eqns is the data-structured returned by distribute_constructor_equations.
       This method selects the ones such that eqns[i].first == C.
       It also updates eqns[i].second.m_renames using \c renaming.
       It also "replaces" the next pattern (a constructor) with its fields.

       The map \c renaming is produced by the `cases` tactic.
       It is needed because the `cases` tactic may revert and reintroduce hypothesis that
       depend on the hypothesis being destructed.

       The parameter \c field should be interpreted as a bit-mask here.
       It says which constructor fields should be used. That is, "some" value means the field
       should be considered.
    */
    list<equation> get_equations_for(name const & C, list<optional<name>> const & fields, name_map<name> const & renaming,
                                     local_context const & lctx, buffer<pair<name, equation>> const & eqns) {
        buffer<equation> R;
        for (auto p : eqns) {
            if (p.first == C) {
                equation eqn  = p.second;
                /* Update renames */
                eqn.m_renames = map(eqn.m_renames, [&](pair<expr, expr> const & p) {
                        if (auto new_name = renaming.find(mlocal_name(p.second))) {
                            return mk_pair(p.first, lctx.get_local_decl(*new_name)->mk_ref());
                        } else {
                            return p;
                        }
                    });
                /* Update patterns */
                type_context ctx = mk_type_context(eqn.m_lctx);
                lean_assert(eqn.m_patterns);
                expr pattern = head(eqn.m_patterns);
                buffer<expr> pattern_args;
                DEBUG_CODE(expr const & C2 =) get_app_args(pattern, pattern_args);
                lean_assert(const_name(C2) == C);
                /* The inductive datatype parameters are always ignored. */
                name I = *eqns_env_interface(m_env).is_constructor(C);
                unsigned I_nparams = eqns_env_interface(m_env).get_inductive_num_params(I);
                lean_assert(I_nparams <= pattern_args.size());
                lean_assert(I_nparams + length(fields) == pattern_args.size());
                buffer<expr> new_patterns;
                auto it_fields = fields;
                for (unsigned i = I_nparams; i < pattern_args.size(); i++) {
                    if (head(it_fields)) {
                        new_patterns.push_back(whnf_pattern(ctx, pattern_args[i]));
                    }
                    it_fields = tail(it_fields);
                }
                eqn.m_patterns = to_list(new_patterns.begin(), new_patterns.end(), tail(eqn.m_patterns));
                R.push_back(eqn);
            }
        }
        return to_list(R);
    }

    /* Store in R the local_decl_refs for ilist by using the local context of the metavariable mvar. */
    void to_buffer_local(expr const & mvar, list<optional<name>> const & ilist, buffer<expr> & R) {
        local_context lctx = get_local_context(mvar);
        for (optional<name> const & x_name : ilist) {
            if (x_name)
                R.push_back(lctx.get_local_decl(*x_name)->mk_ref());
        }
    }

    /* Update an equation left-hand-side of the form

           (f a_1 ... a_n b_1 ... b_m)

       where n == nfields and n+m == arity, with

           (new_fn (c a_1 ... a_n) b_1 ... b_m) */
    expr update_eqn_for_constructor_transition(expr const & eqn, unsigned arity, expr const & new_fn, expr const & c, unsigned nfields) {
        return update_eqn_lhs(eqn, arity, [&](buffer<expr> & args) {
                lean_assert(args.size() >= nfields);
                expr c_app = mk_rev_app(c, nfields, args.end() - nfields);
                args.shrink(args.size() - nfields);
                args.push_back(c_app);
                return mk_rev_app(new_fn, args);
            });
    }

    result compile_constructor(program const & P) {
        trace_match(tout() << "constructor transition\n";);
        lean_assert(is_constructor_transition(P));
        buffer<name> new_names;
        optional<expr> aux_mvar1 = intron(m_env, m_opts, m_mctx, P.m_goal, 1, new_names);
        if (!aux_mvar1) throw_ill_formed_eqns();
        expr x             = get_local_context(*aux_mvar1).get_local_decl(new_names[0])->mk_ref();
        cintros_list ilist;
        renaming_list rlist;
        list<expr> new_goals; list<name> new_goal_cnames;
        try {
            list<name> ids;
            std::tie(new_goals, new_goal_cnames) =
                cases(m_env, m_opts, transparency_mode::Semireducible, m_mctx,
                      *aux_mvar1, x, ids, &ilist, &rlist);
            lean_assert(length(new_goals) == length(new_goal_cnames));
            lean_assert(length(new_goals) == length(ilist));
            lean_assert(length(new_goals) == length(rlist));
        } catch (exception &) {
            trace_match(tout() << "dependent pattern matching step failed\n";);
            throw_error("equation compiler failed (use 'set_option trace.eqn_compiler.elim_match true' "
                        "for additional details)");
        }
        if (empty(new_goals)) {
            return result(m_mctx.instantiate_mvars(P.m_goal));
        } else {
            buffer<pair<name, equation>> equations_by_constructor;
            distribute_constructor_equations(P.m_equations, equations_by_constructor);
            /* For each (reachable) case, we invoke compile recursively, and we store
               - name of the constructor
               - number of fields of this constructor
               - "arity" of the auxiliary program being used in the recursive call
               - result of the compilation for this auxiliary function. */
            buffer<std::tuple<name, unsigned, unsigned, result>> result_by_constructor;
            while (new_goals) {
                lean_assert(new_goal_cnames && ilist && rlist);
                program new_P;
                new_P.m_fn_name = name(P.m_fn_name, head(new_goal_cnames).get_string());
                expr new_goal   = head(new_goals);
                /* Revert constructor fields (which have not been eliminated by dependent pattern matching). */
                buffer<expr> to_revert;
                to_buffer_local(new_goal, head(ilist), to_revert);
                unsigned to_revert_size = to_revert.size();
                unsigned nfields        = to_revert_size;
                expr aux_mvar2          = revert(m_env, m_opts, m_mctx, head(new_goals), to_revert);
                lean_assert(to_revert.size() == to_revert_size);
                new_P.m_goal      = aux_mvar2;
                /* The arity of the auxiliary program is the arity of the original program
                   - 1 (we consumed one argument in this step) and + nfields (we added nfields new arguments). */
                new_P.m_nvars     = P.m_nvars - 1 + nfields;
                new_P.m_equations = get_equations_for(head(new_goal_cnames), head(ilist), head(rlist),
                                                      get_local_context(aux_mvar2), equations_by_constructor);
                result new_R = compile_core(new_P);
                result_by_constructor.emplace_back(head(new_goal_cnames), nfields, new_P.m_nvars, new_R);

                new_goals       = tail(new_goals);
                new_goal_cnames = tail(new_goal_cnames);
                ilist           = tail(ilist);
                rlist           = tail(rlist);
            }
            result new_R;
            new_R.m_code = m_mctx.instantiate_mvars(P.m_goal);
            if (m_lemmas) {
                type_context ctx = mk_type_context(get_local_context(*aux_mvar1));
                expr I           = ctx.infer(x);
                buffer<expr> I_params;
                levels I_lvls    = get_inductive_levels_and_params(ctx, I, I_params);
                buffer<lemma> new_lemmas;
                for (std::tuple<name, unsigned, unsigned, result> const & entry : result_by_constructor) {
                    name const & cname = std::get<0>(entry); /* constructor name */
                    unsigned nfields   = std::get<1>(entry);
                    unsigned arity     = std::get<2>(entry);
                    result const & Rc  = std::get<3>(entry);
                    expr c             = mk_app(mk_constant(cname, I_lvls), I_params);
                    for (lemma const & L : Rc.m_lemmas) {
                        lemma new_L;
                        new_L.m_vars  = L.m_vars;
                        new_L.m_eqn   = update_eqn_for_constructor_transition(L.m_eqn, arity, new_R.m_code, c, nfields);
                        new_L.m_proof = L.m_proof;
                        new_lemmas.push_back(new_L);
                    }
                }
                trace_lemmas(P, "constructor transition", new_lemmas);
                new_R.m_lemmas = to_list(new_lemmas);
            }
            return new_R;
        }
    }

    result compile_value(program const & P) {
        trace_match(tout() << "value+variable transition\n";);
        lean_unreachable();
    }

    result compile_complete(program const & P) {
        trace_match(tout() << "complete transition\n";);
        lean_unreachable();
    }

    result compile_leaf(program const & P) {
        if (!P.m_equations) {
            throw_error("invalid non-exhaustive set of equations (use 'set_option trace.eqn_compiler.elim_match true' "
                        "for additional details)");
        }
        equation const & eqn   = head(P.m_equations);
        m_used_eqns[eqn.m_idx] = true;
        buffer<expr> from, to;
        for (pair<expr, expr> const & p : eqn.m_renames) {
            from.push_back(p.first);
            to.push_back(p.second);
        }
        expr rhs = replace_locals(eqn.m_rhs, from, to);
        m_mctx.assign(P.m_goal, rhs);
        result R;
        R.m_code = rhs;
        if (m_lemmas) {
            type_context ctx = mk_type_context(get_local_context(P));
            expr eq    = mk_eq(ctx, rhs, rhs);
            expr H     = mk_eq_refl(ctx, rhs);
            R.m_lemmas = to_list(lemma(list<expr>(), eq, H));
        }
        return R;
    }

    result compile_core(program const & P) {
        flet<unsigned> inc_depth(m_depth, m_depth+1);
        trace_match(tout() << "depth [" << m_depth << "]\n" << pp_program(P) << "\n";);
        if (P.m_nvars > 0) {
            if (!P.m_equations) {
                return compile_no_equation(P);
            } else if (is_inaccessible_transition(P)) {
                return compile_skip(P);
            } else if (is_variable_transition(P)) {
                return compile_variable(P);
            } else if (is_constructor_transition(P)) {
                return compile_constructor(P);
            } else if (is_value_transition(P)) {
                return compile_value(P);
            } else if (is_complete_transition(P)) {
                return compile_complete(P);
            } else if (some_inaccessible(P)) {
                throw_error("invalid equations, inconsistent use of inaccessible term annotation, "
                            "in some equations pattern is an inaccessible term and in others it is not");
            } else {
                trace_match(tout() << "compilation faild at\n" << pp_program(P) << "\n";);
                throw_error("equation compiler failed (use 'set_option trace.eqn_compiler.elim_match true' "
                            "for additional details)");
            }
        } else {
            return compile_leaf(P);
        }
    }

    void check_all_equations_used() {
        for (unsigned i = 0; i < m_used_eqns.size(); i++) {
            if (!m_used_eqns[i]) {
                throw_error(sstream() << "equation #" << (i+1) << " is redundant");
            }
        }
    }

    result compile(program const & P) {
        result R = compile_core(P);
        check_all_equations_used();
        return R;
    }

    expr operator()(local_context const & lctx, expr const & eqns) {
        lean_assert(equations_num_fns(eqns) == 1);
        DEBUG_CODE({
                type_context ctx = mk_type_context(lctx);
                lean_assert(!is_recursive_eqns(ctx, eqns));
            });
        m_ref     = eqns;
        program P = mk_program(lctx, eqns);
        result R  = compile(P);

        lean_unreachable();
    }
};

expr elim_match(environment & env, options const & opts, metavar_context & mctx,
                local_context const & lctx, expr const & eqns) {
    return elim_match_fn(env, opts, mctx)(lctx, eqns);
}

void initialize_elim_match() {
    register_trace_class({"eqn_compiler", "elim_match"});
    register_trace_class({"eqn_compiler", "elim_match_detail"});
}
void finalize_elim_match() {
}
}