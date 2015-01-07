/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    spacer_util.cpp

Abstract:

    Utility functions for SPACER.

Author:

    Krystof Hoder (t-khoder) 2011-8-19.

Revision History:

    Modified by Anvesh Komuravelli

Notes: 
    

--*/

#include <sstream>
#include "arith_simplifier_plugin.h"
#include "array_decl_plugin.h"
#include "ast_pp.h"
#include "basic_simplifier_plugin.h"
#include "bv_simplifier_plugin.h"
#include "bool_rewriter.h"
#include "dl_util.h"
#include "for_each_expr.h"
#include "smt_params.h"
#include "model.h"
#include "ref_vector.h"
#include "rewriter.h"
#include "rewriter_def.h"
#include "util.h"
#include "spacer_manager.h"
#include "spacer_util.h"
#include "arith_decl_plugin.h"
#include "expr_replacer.h"
#include "model_smt2_pp.h"
#include "scoped_proof.h"
#include "qe_lite.h"
#include "qe_project.h"
#include "model_pp.h"
#include "expr_safe_replace.h"

#include "datatype_decl_plugin.h"
#include "bv_decl_plugin.h"


namespace spacer {

    unsigned ceil_log2(unsigned u) {
        if (u == 0) { return 0; }
        unsigned pow2 = next_power_of_two(u);
        return get_num_1bits(pow2-1);
    }

    std::string pp_cube(const ptr_vector<expr>& model, ast_manager& m) {
        return pp_cube(model.size(), model.c_ptr(), m);
    }

    std::string pp_cube(const expr_ref_vector& model, ast_manager& m) {
        return pp_cube(model.size(), model.c_ptr(), m);
    }

    std::string pp_cube(const app_ref_vector& model, ast_manager& m) {
        return pp_cube(model.size(), model.c_ptr(), m);
    }
    
    std::string pp_cube(const app_vector& model, ast_manager& m) {
        return pp_cube(model.size(), model.c_ptr(), m);
    }

    std::string pp_cube(unsigned sz, app * const * lits, ast_manager& m) {
        return pp_cube(sz, reinterpret_cast<expr * const *>(lits), m);
    }

    std::string pp_cube(unsigned sz, expr * const * lits, ast_manager& m) {
        std::stringstream res;
        res << "(";
        expr * const * end = lits+sz;
        for (expr * const * it = lits; it!=end; it++) {
            res << mk_pp(*it, m);
            if (it+1!=end) {
                res << ", ";
            }
        }
        res << ")";
        return res.str();
    }



    /////////////////////////
    // model_evaluator
    //
    

    void model_evaluator::assign_value(expr* e, expr* val) {
        rational r;
        if (m.is_true(val)) {
            set_true(e);
        }
        else if (m.is_false(val)) {
            set_false(e);
        }
        else if (m_arith.is_numeral(val, r)) {
            set_number(e, r);
        }
        else if (m.is_value(val)) {
            set_value(e, val);
        }
        else {
            IF_VERBOSE(3, verbose_stream() << "Not evaluated " << mk_pp(e, m) << "\n";);
            TRACE("spacer", tout << "Variable is not tracked: " << mk_pp(e, m) << "\n";);
            set_x(e);
        }
    }

    void model_evaluator::setup_model(model_ref& model) {
        m_numbers.reset();
        m_values.reset();
        m_model = model;
        rational r;
        unsigned sz = model->get_num_constants();
        for (unsigned i = 0; i < sz; i++) {
            func_decl * d = model->get_constant(i); 
            expr* val = model->get_const_interp(d);
            expr* e = m.mk_const(d);
            m_refs.push_back(e);
            assign_value(e, val);
        }
    }
    
    void model_evaluator::reset() {
        m1.reset();
        m2.reset();
        m_values.reset();
        m_visited.reset();
        m_numbers.reset();
        m_refs.reset();
        m_model = 0;
    }
    
    expr_ref_vector model_evaluator::minimize_model(ptr_vector<expr> const & formulas, model_ref& mdl) {
        setup_model(mdl);
        
        TRACE("spacer_verbose", 
              tout << "formulas:\n";
              for (unsigned i = 0; i < formulas.size(); ++i) tout << mk_pp(formulas[i], m) << "\n"; 
              );
        
        expr_ref_vector model = prune_by_cone_of_influence(formulas);
        TRACE("spacer_verbose",
              tout << "pruned model:\n";
              for (unsigned i = 0; i < model.size(); ++i) tout << mk_pp(model[i].get(), m) << "\n";);
        
        reset();
        
        DEBUG_CODE(
            setup_model(mdl);
            VERIFY(check_model(formulas));
            reset(););
        
        return model;
    }
    
    void model_evaluator::minimize_literals(ptr_vector<expr> const& formulas, model_ref& mdl, expr_ref_vector& result) {
        
        TRACE("spacer", 
              tout << "formulas:\n";
              for (unsigned i = 0; i < formulas.size(); ++i) tout << mk_pp(formulas[i], m) << "\n"; 
              );
        
        expr_ref tmp(m);
        ptr_vector<expr> tocollect;
        
        setup_model(mdl);
        collect(formulas, tocollect);
        for (unsigned i = 0; i < tocollect.size(); ++i) {
            expr* e = tocollect[i];
            expr* e1, *e2;
            SASSERT(m.is_bool(e));
            SASSERT(is_true(e) || is_false(e));
            if (is_true(e)) {
                result.push_back(e);
            }
            // hack to break disequalities for arithmetic variables.
            else if (m.is_eq(e, e1, e2) && m_arith.is_int_real(e1)) {
                if (get_number(e1) < get_number(e2)) {
                    result.push_back(m_arith.mk_lt(e1,e2));
                }
                else {
                    result.push_back(m_arith.mk_lt(e2,e1));
                }
            }
            else {
                result.push_back(m.mk_not(e));
            }
        }
        reset();
        TRACE("spacer", 
              tout << "minimized model:\n";
              for (unsigned i = 0; i < result.size(); ++i) tout << mk_pp(result[i].get(), m) << "\n"; 
              );
    }
    
    void model_evaluator::process_formula(app* e, ptr_vector<expr>& todo, ptr_vector<expr>& tocollect) {
        SASSERT(m.is_bool(e));
        SASSERT(is_true(e) || is_false(e));
        unsigned v = is_true(e);
        unsigned sz = e->get_num_args();
        expr* const* args = e->get_args();
        if (e->get_family_id() == m.get_basic_family_id()) {
            switch(e->get_decl_kind()) {
            case OP_TRUE:
                break;
            case OP_FALSE:
                break;
            case OP_EQ:
            case OP_IFF:
                if (args[0] == args[1]) {
                    SASSERT(v);
                    // no-op                    
                }
                else if (m.is_bool(args[0])) {
                    todo.append(sz, args);
                }
                else {
                    tocollect.push_back(e);
                }
                break;                              
            case OP_DISTINCT:
                tocollect.push_back(e);
                break;
            case OP_ITE:
                if (args[1] == args[2]) {
                    tocollect.push_back(args[1]);
                }
                else if (is_true(args[1]) && is_true(args[2])) {
                    todo.append(2, args+1);
                }
                else if (is_false(args[1]) && is_false(args[2])) {
                    todo.append(2, args+1);
                }
                else if (is_true(args[0])) {
                    todo.append(2, args);
                }
                else {
                    SASSERT(is_false(args[0]));
                    todo.push_back(args[0]);
                    todo.push_back(args[2]);
                }
                break;
            case OP_AND:
                if (v) {
                    todo.append(sz, args);
                }
                else {
                    unsigned i = 0;
                    for (; !is_false(args[i]) && i < sz; ++i);     
                    if (i == sz) {
                        fatal_error(1);
                    }
                    VERIFY(i < sz);
                    todo.push_back(args[i]);
                }
                break;
            case OP_OR:
                if (v) {
                    unsigned i = 0;
                    for (; !is_true(args[i]) && i < sz; ++i);
                    if (i == sz) {
                        fatal_error(1);
                    }
                    VERIFY(i < sz);
                    todo.push_back(args[i]);
                }
                else {
                    todo.append(sz, args);
                }
                break;
            case OP_XOR: 
            case OP_NOT:
                todo.append(sz, args);
                break;
            case OP_IMPLIES:
                if (v) {
                    if (is_true(args[1])) {
                        todo.push_back(args[1]);
                    }
                    else if (is_false(args[0])) {
                        todo.push_back(args[0]);
                    }
                    else {
                        IF_VERBOSE(0, verbose_stream() << "Term not handled " << mk_pp(e, m) << "\n";);
                        UNREACHABLE();
                    }
                }
                else {
                    todo.append(sz, args);
                }
                break;
            default:
                IF_VERBOSE(0, verbose_stream() << "Term not handled " << mk_pp(e, m) << "\n";);
                UNREACHABLE();
            }
        }
        else {
            tocollect.push_back(e);
        }
    }
    
    void model_evaluator::collect(ptr_vector<expr> const& formulas, ptr_vector<expr>& tocollect) {
        ptr_vector<expr> todo;
        todo.append(formulas);
        m_visited.reset();
        
        VERIFY(check_model(formulas));
        
        while (!todo.empty()) {
            app*  e = to_app(todo.back());
            todo.pop_back();
            if (!m_visited.is_marked(e)) {
                process_formula(e, todo, tocollect);
                m_visited.mark(e, true);
            }
        }
        m_visited.reset();
    }
    
    expr_ref_vector model_evaluator::prune_by_cone_of_influence(ptr_vector<expr> const & formulas) {
        ptr_vector<expr> tocollect;
        collect(formulas, tocollect);
        m1.reset();
        m2.reset();
        for (unsigned i = 0; i < tocollect.size(); ++i) {     
            TRACE("spacer_verbose", tout << "collect: " << mk_pp(tocollect[i], m) << "\n";);
            for_each_expr(*this, m_visited, tocollect[i]);
        }
        unsigned sz = m_model->get_num_constants();
        expr_ref e(m), eq(m), val(m);
        expr_ref_vector model(m);
        for (unsigned i = 0; i < sz; i++) {
            e = m.mk_const(m_model->get_constant(i));
            if (m_visited.is_marked(e)) {
                val = eval(m_model, e);
                eq = m.mk_eq(e, val);
                model.push_back(eq);
            }
        }
        m_visited.reset();
        TRACE("spacer", tout << sz << " ==> " << model.size() << "\n";);
        return model;
        
    }
    
    void model_evaluator::eval_arith(app* e) {
        rational r, r2;
        
#define ARG1 e->get_arg(0)
#define ARG2 e->get_arg(1)     
        
        unsigned arity = e->get_num_args();
        for (unsigned i = 0; i < arity; ++i) {
            expr* arg = e->get_arg(i);
            if (is_x(arg)) {
                set_x(e);
                return;
            }
            SASSERT(!is_unknown(arg));
        }
        switch(e->get_decl_kind()) {
        case OP_NUM: 
            VERIFY(m_arith.is_numeral(e, r));
            set_number(e, r);
            break;                
        case OP_IRRATIONAL_ALGEBRAIC_NUM:  
            set_x(e);
            break;
        case OP_LE:
            set_bool(e, get_number(ARG1) <= get_number(ARG2));
            break;
        case OP_GE:
            set_bool(e, get_number(ARG1) >= get_number(ARG2));
            break;
        case OP_LT:
            set_bool(e, get_number(ARG1) < get_number(ARG2));
            break;
        case OP_GT:
            set_bool(e, get_number(ARG1) > get_number(ARG2));
            break;
        case OP_ADD: 
            r = rational::zero();
            for (unsigned i = 0; i < arity; ++i) {
                r += get_number(e->get_arg(i));
            }
            set_number(e, r);
            break;                                    
        case OP_SUB: 
            r = get_number(e->get_arg(0));
            for (unsigned i = 1; i < arity; ++i) {
                r -= get_number(e->get_arg(i));
            }
            set_number(e, r);
            break;                            
        case OP_UMINUS: 
            SASSERT(arity == 1);
            set_number(e, -get_number(e->get_arg(0)));
            break;                
        case OP_MUL: 
            r = rational::one();
            for (unsigned i = 0; i < arity; ++i) {
                r *= get_number(e->get_arg(i));
            }
            set_number(e, r);
            break;                
        case OP_DIV: 
            SASSERT(arity == 2);
            r = get_number(ARG2);
            if (r.is_zero()) {
                set_x(e);
            }
            else {
                set_number(e, get_number(ARG1) / r);
            }
            break;                
        case OP_IDIV: 
            SASSERT(arity == 2);
            r = get_number(ARG2);
            if (r.is_zero()) {
                set_x(e);
            }
            else {
                set_number(e, div(get_number(ARG1), r));
            }
            break;                
        case OP_REM: 
            // rem(v1,v2) = if v2 >= 0 then mod(v1,v2) else -mod(v1,v2)
            SASSERT(arity == 2);
            r = get_number(ARG2);
            if (r.is_zero()) {
                set_x(e);
            }
            else {
                r2 = mod(get_number(ARG1), r);
                if (r.is_neg()) r2.neg();
                set_number(e, r2);
            }
            break;
        case OP_MOD: 
            SASSERT(arity == 2);
            r = get_number(ARG2);
            if (r.is_zero()) {
                set_x(e);
            }
            else {
                set_number(e, mod(get_number(ARG1), r));
            }
            break;                   
        case OP_TO_REAL: 
            SASSERT(arity == 1);
            set_number(e, get_number(ARG1));
            break;                
        case OP_TO_INT: 
            SASSERT(arity == 1);
            set_number(e, floor(get_number(ARG1)));
            break;                
        case OP_IS_INT: 
            SASSERT(arity == 1);
            set_bool(e, get_number(ARG1).is_int());
            break;
        case OP_POWER:
            set_x(e);
            break;
        default:
            IF_VERBOSE(0, verbose_stream() << "Term not handled " << mk_pp(e, m) << "\n";);
            UNREACHABLE();
            break;
        }
    }
    
    void model_evaluator::inherit_value(expr* e, expr* v) {
        expr* w;
        SASSERT(!is_unknown(v));
        SASSERT(m.get_sort(e) == m.get_sort(v));
        if (is_x(v)) {
            set_x(e);
        }
        else if (m.is_bool(e)) {
            SASSERT(m.is_bool(v));
            if (is_true(v)) set_true(e);
            else if (is_false(v)) set_false(e);
            else {
                TRACE("spacer", tout << "not inherited:\n" << mk_pp(e, m) << "\n" << mk_pp(v, m) << "\n";);
                set_x(e);
            }
        }
        else if (m_arith.is_int_real(e)) {
            set_number(e, get_number(v));
        }
        else if (m.is_value(v)) {
            set_value(e, v);
        }
        else if (m_values.find(v, w)) {
            set_value(e, w);
        }
        else {
            TRACE("spacer", tout << "not inherited:\n" << mk_pp(e, m) << "\n" << mk_pp(v, m) << "\n";);
            set_x(e);
        }
    }

    void model_evaluator::eval_exprs(expr_ref_vector& es) {
        model_ref mr(m_model);
        for (unsigned j = 0; j < es.size(); ++j) {
            if (m_array.is_as_array(es[j].get())) {
                es[j] = eval(mr, es[j].get());
            }
        }
    }
    
    bool model_evaluator::extract_array_func_interp(expr* a, vector<expr_ref_vector>& stores, expr_ref& else_case) {
        SASSERT(m_array.is_array(a));
        
        TRACE("spacer", tout << mk_pp(a, m) << "\n";);
        while (m_array.is_store(a)) {
            expr_ref_vector store(m);
            store.append(to_app(a)->get_num_args()-1, to_app(a)->get_args()+1);
            eval_exprs(store);
            stores.push_back(store);
            a = to_app(a)->get_arg(0);
        }
        
        if (m_array.is_const(a)) {
            else_case = to_app(a)->get_arg(0);
            return true;
        }
        
        while (m_array.is_as_array(a)) {
            func_decl* f = m_array.get_as_array_func_decl(to_app(a));
            func_interp* g = m_model->get_func_interp(f);
            unsigned sz = g->num_entries();
            unsigned arity = f->get_arity();
            for (unsigned i = 0; i < sz; ++i) {
                expr_ref_vector store(m);
                func_entry const* fe = g->get_entry(i);
                store.append(arity, fe->get_args());
                store.push_back(fe->get_result());
                for (unsigned j = 0; j < store.size(); ++j) {
                    if (!is_ground(store[j].get())) {
                        TRACE("spacer", tout << "could not extract array interpretation: " << mk_pp(a, m) << "\n" << mk_pp(store[j].get(), m) << "\n";);
                        return false;
                    }
                }
                eval_exprs(store);
                stores.push_back(store);
            }        
            else_case = g->get_else();
            if (!else_case) {
                TRACE("spacer", tout << "no else case " << mk_pp(a, m) << "\n";);
                return false;
            }
            if (!is_ground(else_case)) {
                TRACE("spacer", tout << "non-ground else case " << mk_pp(a, m) << "\n" << mk_pp(else_case, m) << "\n";);
                return false;
            }
            if (m_array.is_as_array(else_case)) {
                model_ref mr(m_model);
                else_case = eval(mr, else_case);
            }
            TRACE("spacer", tout << "else case: " << mk_pp(else_case, m) << "\n";);
            return true;
        }
        TRACE("spacer", tout << "no translation: " << mk_pp(a, m) << "\n";);
        
        return false;
    }
    
    /**
       best effort evaluator of extensional array equality.
     */
    void model_evaluator::eval_array_eq(app* e, expr* arg1, expr* arg2) {
        TRACE("spacer", tout << "array equality: " << mk_pp(e, m) << "\n";);
        expr_ref v1(m), v2(m);
        m_model->eval(arg1, v1);
        m_model->eval(arg2, v2);
        if (v1 == v2) {
            set_true(e);
            return;
        }
        sort* s = m.get_sort(arg1);
        sort* r = get_array_range(s);
        // give up evaluating finite domain/range arrays
        if (!r->is_infinite() && !r->is_very_big() && !s->is_infinite() && !s->is_very_big()) {
            TRACE("spacer", tout << "equality is unknown: " << mk_pp(e, m) << "\n";);
            set_x(e);
            return;
        }
        vector<expr_ref_vector> store;
        expr_ref else1(m), else2(m);
        if (!extract_array_func_interp(v1, store, else1) ||
            !extract_array_func_interp(v2, store, else2)) {
            TRACE("spacer", tout << "equality is unknown: " << mk_pp(e, m) << "\n";);
            set_x(e);
            return;
        }

        if (else1 != else2) {
            if (m.is_value(else1) && m.is_value(else2)) {
                TRACE("spacer", tout 
                      << "defaults are different: " << mk_pp(e, m) << " " 
                      << mk_pp(else1, m) << " " << mk_pp(else2, m) << "\n";);
                set_false(e);
            }
            else if (m_array.is_array(else1)) {
                eval_array_eq(e, else1, else2);
            }
            else {
                TRACE("spacer", tout << "equality is unknown: " << mk_pp(e, m) << "\n";);
                set_x(e);
            }
            return;
        }

        expr_ref s1(m), s2(m), w1(m), w2(m);        
        expr_ref_vector args1(m), args2(m);
        args1.push_back(v1);
        args2.push_back(v2);        
        for (unsigned i = 0; i < store.size(); ++i) {
            args1.resize(1);
            args2.resize(1);
            args1.append(store[i].size()-1, store[i].c_ptr());
            args2.append(store[i].size()-1, store[i].c_ptr());
            s1 = m_array.mk_select(args1.size(), args1.c_ptr());
            s2 = m_array.mk_select(args2.size(), args2.c_ptr());
            m_model->eval(s1, w1);
            m_model->eval(s2, w2);
            if (w1 == w2) {
                continue;
            }
            if (m.is_value(w1) && m.is_value(w2)) {
                TRACE("spacer", tout << "Equality evaluation: " << mk_pp(e, m) << "\n"; 
                      tout << mk_pp(s1, m) << " |-> " << mk_pp(w1, m) << "\n";
                      tout << mk_pp(s2, m) << " |-> " << mk_pp(w2, m) << "\n";);
                set_false(e);
            }
            else if (m_array.is_array(w1)) {
                eval_array_eq(e, w1, w2);
                if (is_true(e)) {
                    continue;
                }
            }
            else {
                TRACE("spacer", tout << "equality is unknown: " << mk_pp(e, m) << "\n";);
                set_x(e);
            }
            return;
        }
        set_true(e);
    }

    void model_evaluator::eval_eq(app* e, expr* arg1, expr* arg2) {
        if (arg1 == arg2) {
            set_true(e);
        }
        else if (m_array.is_array(arg1)) {
            eval_array_eq(e, arg1, arg2);
        }
        else if (is_x(arg1) || is_x(arg2)) {
            set_x(e);
        }
        else if (m.is_bool(arg1)) {
            bool val = is_true(arg1) == is_true(arg2);
            SASSERT(val == (is_false(arg1) == is_false(arg2)));
            if (val) {
                set_true(e);
            }
            else {
                set_false(e);
            }            
        }
        else if (m_arith.is_int_real(arg1)) {
            set_bool(e, get_number(arg1) == get_number(arg2));
        }
        else {
            expr* e1 = get_value(arg1);
            expr* e2 = get_value(arg2);
            if (m.is_value(e1) && m.is_value(e2)) {
                set_bool(e, e1 == e2);
            }
            else if (e1 == e2) {
                set_bool(e, true);
            }
            else {
                TRACE("spacer", tout << "not value equal:\n" << mk_pp(e1, m) << "\n" << mk_pp(e2, m) << "\n";);
                set_x(e);
            }
        }
    }

    void model_evaluator::eval_basic(app* e) {
        expr* arg1, *arg2;
        expr *argCond, *argThen, *argElse, *arg;
        bool has_x = false;
        unsigned arity = e->get_num_args();
        switch(e->get_decl_kind()) {
        case OP_AND: 
            for (unsigned j = 0; j < arity; ++j) {
                expr * arg = e->get_arg(j);
                if (is_false(arg)) {
                    set_false(e);
                    return;
                }
                else if (is_x(arg)) {
                    has_x = true;
                }
                else {
                    SASSERT(is_true(arg));
                }
            }
            if (has_x) {
                set_x(e);
            }
            else {
                set_true(e);
            }
            break;
        case OP_OR: 
            for (unsigned j = 0; j < arity; ++j) {
                expr * arg = e->get_arg(j);
                if (is_true(arg)) {
                    set_true(e);
                    return;
                }
                else if (is_x(arg)) {
                    has_x = true;
                }
                else {
                    SASSERT(is_false(arg));
                }
            }
            if (has_x) {
                set_x(e);
            }
            else {
                set_false(e);
            }
            break;
        case OP_NOT: 
            VERIFY(m.is_not(e, arg));
            if (is_true(arg)) {
                set_false(e);
            }
            else if (is_false(arg)) {
                set_true(e);
            }
            else {
                SASSERT(is_x(arg));
                set_x(e);
            }
            break;
        case OP_IMPLIES: 
            VERIFY(m.is_implies(e, arg1, arg2));
            if (is_false(arg1) || is_true(arg2)) {
                set_true(e);
            }
            else if (arg1 == arg2) {
                set_true(e);
            }
            else if (is_true(arg1) && is_false(arg2)) {
                set_false(e);
            }
            else {
                SASSERT(is_x(arg1) || is_x(arg2));
                set_x(e);
            }
            break;
        case OP_IFF: 
            VERIFY(m.is_iff(e, arg1, arg2));
            eval_eq(e, arg1, arg2);
            break;
        case OP_XOR:
          VERIFY (m.is_xor (e, arg1, arg2));
          eval_eq (e, arg1, arg2);
          if (is_false (e)) set_true (e);
          else if (is_true (e)) set_false (e);
          break;
        case OP_ITE: 
            VERIFY(m.is_ite(e, argCond, argThen, argElse));
            if (is_true(argCond)) { 
                inherit_value(e, argThen);
            }
            else if (is_false(argCond)) {
                inherit_value(e, argElse);
            }
            else if (argThen == argElse) {
                inherit_value(e, argThen);
            }
            else if (m.is_bool(e)) {
                SASSERT(is_x(argCond));
                if (is_x(argThen) || is_x(argElse)) {
                    set_x(e);
                }
                else if (is_true(argThen) == is_true(argElse)) {
                    inherit_value(e, argThen);
                }
                else {
                    set_x(e);
                }
            }
            else {
                set_x(e);
            }
            break;
        case OP_TRUE:
            set_true(e);
            break;
        case OP_FALSE:
            set_false(e);
            break;
        case OP_EQ:
            VERIFY(m.is_eq(e, arg1, arg2));
            eval_eq(e, arg1, arg2);
            break;
        case OP_DISTINCT: {
            vector<rational> values;
            for (unsigned i = 0; i < arity; ++i) {
                expr* arg = e->get_arg(i);
                if (is_x(arg)) {
                    set_x(e);
                    return;
                }
                values.push_back(get_number(arg));
            }
            std::sort(values.begin(), values.end());
            for (unsigned i = 0; i + 1 < values.size(); ++i) {
                if (values[i] == values[i+1]) {
                    set_false(e);
                    return;
                }
            }
            set_true(e);
            break;
        }
        default:
            IF_VERBOSE(0, verbose_stream() << "Term not handled " << mk_pp(e, m) << "\n";);
            UNREACHABLE();        
        }
    }

    void model_evaluator::eval_fmls (ptr_vector<expr> const& formulas) {
        ptr_vector<expr> todo(formulas);
        
        while (!todo.empty()) {
            expr * curr_e = todo.back();
            
            if (!is_app(curr_e)) { 
                todo.pop_back();
                continue;
            }
            app * curr = to_app(curr_e);
            
            if (!is_unknown(curr)) { 
                todo.pop_back();
                continue;
            }
            unsigned arity = curr->get_num_args();
            for (unsigned i = 0; i < arity; ++i) {
                if (is_unknown(curr->get_arg(i))) {
                    todo.push_back(curr->get_arg(i));
                }
            }
            if (todo.back() != curr) {
                continue;
            }
            todo.pop_back();
            if (curr->get_family_id() == m_arith.get_family_id()) {
                eval_arith(curr);
            }
            else if (curr->get_family_id() == m.get_basic_family_id()) {
                eval_basic(curr);
            }
            else {
                expr_ref vl(m);
                m_model->eval(curr, vl);
                assign_value(curr, vl);
            }
            
            IF_VERBOSE(35,verbose_stream() << "assigned "<<mk_pp(curr_e,m) 
                       <<(is_true(curr_e) ? "true" : is_false(curr_e) ? "false" : "unknown") << "\n";);
            SASSERT(!is_unknown(curr));
        }
    }
    
    bool model_evaluator::check_model(ptr_vector<expr> const& formulas) {
        eval_fmls (formulas);
        bool has_x = false;
        for (unsigned i = 0; i < formulas.size(); ++i) {
            expr * form = formulas[i];
            SASSERT(!is_unknown(form));
            TRACE("spacer_verbose", 
                  tout << "formula is " << (is_true(form) ? "true" : is_false(form) ? "false" : "unknown") << "\n" <<mk_pp(form, m)<< "\n";);
            
            if (is_false(form)) {
                IF_VERBOSE(0, verbose_stream() << "formula false in model: " << mk_pp(form, m) << "\n";);
                UNREACHABLE();
            }
            if (is_x(form)) {
                IF_VERBOSE(0, verbose_stream() << "formula undetermined in model: " << mk_pp(form, m) << "\n";);
                TRACE("spacer", model_smt2_pp(tout, m, *m_model, 0);); 
                has_x = true;
            }
        }
        return !has_x;
    }

    void model_evaluator::eval_heavy (model_ref& model, expr* fml, expr_ref& result) {
        setup_model (model);
        ptr_vector<expr> fmls; fmls.push_back (fml);
        eval_fmls (fmls);

        SASSERT (!is_unknown (fml));
        if (is_x (fml)) {
            result = fml;
        }
        else if (m.is_bool (fml)) {
            result = is_false (fml) ? m.mk_false () : m.mk_true ();
        }
        else if (m_arith.is_int_real (fml)) {
            result = m_arith.mk_numeral (get_number (fml), m_arith.is_int (fml));
        }
        else {
            result = get_value (fml);
        }
        reset ();
    }

    expr_ref model_evaluator::eval(model_ref& model, func_decl* d) {
        SASSERT(d->get_arity() == 0);
        expr_ref result(m);
        if (m_array.is_array(d->get_range())) {
            expr_ref e(m);
            e = m.mk_const(d);
            result = eval(model, e);
        }
        else {
            result = model->get_const_interp(d);
        }
        return result;
    }

    expr_ref model_evaluator::eval(model_ref& model, expr* e) {
        expr_ref result(m);
        m_model = model;
        VERIFY(m_model->eval(e, result, true));
        if (m_array.is_array(e)) {
            vector<expr_ref_vector> stores;
            expr_ref_vector args(m);
            expr_ref else_case(m);
            if (extract_array_func_interp(result, stores, else_case)) {
                result = m_array.mk_const_array(m.get_sort(e), else_case);
                while (!stores.empty() && stores.back().back() == else_case) {
                    stores.pop_back();
                }
                for (unsigned i = stores.size(); i > 0; ) {
                    --i;
                    args.resize(1);
                    args[0] = result;
                    args.append(stores[i]);
                    result = m_array.mk_store(args.size(), args.c_ptr());
                }
                return result;
            }
        }
        return result;
    }


    void reduce_disequalities(model& model, unsigned threshold, expr_ref& fml) {
        ast_manager& m = fml.get_manager();
        expr_ref_vector conjs(m);
        qe::flatten_and(fml, conjs);
        obj_map<expr, unsigned> diseqs;
        expr* n, *lhs, *rhs;
        for (unsigned i = 0; i < conjs.size(); ++i) {
            if (m.is_not(conjs[i].get(), n) &&
                m.is_eq(n, lhs, rhs)) {
                if (!m.is_value(rhs)) {
                    std::swap(lhs, rhs);
                }
                if (!m.is_value(rhs)) {
                    continue;
                }
                diseqs.insert_if_not_there2(lhs, 0)->get_data().m_value++;
            }
        }
        expr_substitution sub(m);

        unsigned orig_size = conjs.size();
        unsigned num_deleted = 0;
        expr_ref val(m), tmp(m);
        proof_ref pr(m);
        pr = m.mk_asserted(m.mk_true());
        obj_map<expr, unsigned>::iterator it  = diseqs.begin();
        obj_map<expr, unsigned>::iterator end = diseqs.end();
        for (; it != end; ++it) {
            if (it->m_value >= threshold) {
                model.eval(it->m_key, val);
                sub.insert(it->m_key, val, pr);
                conjs.push_back(m.mk_eq(it->m_key, val));
                num_deleted += it->m_value;
            }
        }
        if (orig_size < conjs.size()) {
            scoped_ptr<expr_replacer> rep = mk_expr_simp_replacer(m);
            rep->set_substitution(&sub);
            for (unsigned i = 0; i < orig_size; ++i) {
                tmp = conjs[i].get();
                (*rep)(tmp);
                if (m.is_true(tmp)) {
                    conjs[i] = conjs.back();
                    SASSERT(orig_size <= conjs.size());
                    conjs.pop_back();
                    SASSERT(orig_size <= 1 + conjs.size());
                    if (i + 1 == orig_size) {
                        // no-op.
                    }
                    else if (orig_size <= conjs.size()) {
                        // no-op
                    }
                    else {
                        SASSERT(orig_size == 1 + conjs.size());
                        --orig_size;
                        --i;
                    }
                }
                else {
                    conjs[i] = tmp;
                }
            }            
            IF_VERBOSE(2, verbose_stream() << "Deleted " << num_deleted << " disequalities " << conjs.size() << " conjuncts\n";);
        }
        fml = m.mk_and(conjs.size(), conjs.c_ptr());        
    }

    // 
    // (f (if c1 (if c2 e1 e2) e3) b c) -> 
    // (if c1 (if c2 (f e1 b c)

    class ite_hoister {
        ast_manager& m;
    public:
        ite_hoister(ast_manager& m): m(m) {}

        br_status mk_app_core(func_decl* f, unsigned num_args, expr* const* args, expr_ref& result) {
            if (m.is_ite(f)) {
                return BR_FAILED;
            }
            for (unsigned i = 0; i < num_args; ++i) {
                expr* c, *t, *e;
                if (!m.is_bool(args[i]) && m.is_ite(args[i], c, t, e)) {
                    expr_ref e1(m), e2(m);
                    ptr_vector<expr> args1(num_args, args);
                    args1[i] = t;
                    e1 = m.mk_app(f, num_args, args1.c_ptr());
                    if (t == e) {
                        result = e1;
                        return BR_REWRITE1;
                    }
                    args1[i] = e;
                    e2 = m.mk_app(f, num_args, args1.c_ptr());
                    result = m.mk_app(f, num_args, args);
                    result = m.mk_ite(c, e1, e2);
                    return BR_REWRITE3;
                }
            }
            return BR_FAILED;
        }
    };

    struct ite_hoister_cfg: public default_rewriter_cfg {
        ite_hoister m_r;
        bool rewrite_patterns() const { return false; }
        br_status reduce_app(func_decl * f, unsigned num, expr * const * args, expr_ref & result, proof_ref & result_pr) {
            return m_r.mk_app_core(f, num, args, result);
        }
        ite_hoister_cfg(ast_manager & m, params_ref const & p):m_r(m) {}        
    };

    class ite_hoister_star : public rewriter_tpl<ite_hoister_cfg> {
        ite_hoister_cfg m_cfg;
    public:
        ite_hoister_star(ast_manager & m, params_ref const & p):
            rewriter_tpl<ite_hoister_cfg>(m, false, m_cfg),
            m_cfg(m, p) {}
    };

    void hoist_non_bool_if(expr_ref& fml) {
        ast_manager& m = fml.get_manager();
        scoped_no_proof _sp(m);
        params_ref p;
        ite_hoister_star ite_rw(m, p);
        expr_ref tmp(m);
        ite_rw(fml, tmp);
        fml = tmp;        
    }

    class test_diff_logic {
        ast_manager& m;
        arith_util a;
        bv_util    bv;
        bool m_is_dl;
        bool m_test_for_utvpi;

        bool is_numeric(expr* e) const {
            if (a.is_numeral(e)) {
                return true;
            }
            expr* cond, *th, *el;
            if (m.is_ite(e, cond, th, el)) {
                return is_numeric(th) && is_numeric(el);
            }
            return false;
        }
        
        bool is_arith_expr(expr *e) const {
            return is_app(e) && a.get_family_id() == to_app(e)->get_family_id();
        }

        bool is_offset(expr* e) const {
            if (a.is_numeral(e)) {
                return true;
            }
            expr* cond, *th, *el, *e1, *e2;
            if (m.is_ite(e, cond, th, el)) {
                return is_offset(th) && is_offset(el);
            }
            // recognize offsets.
            if (a.is_add(e, e1, e2)) {
                if (is_numeric(e1)) {
                    return is_offset(e2);
                }
                if (is_numeric(e2)) {
                    return is_offset(e1);
                }
                return false;
            }
            if (m_test_for_utvpi) {
                if (a.is_mul(e, e1, e2)) {
                    if (is_minus_one(e1)) {
                        return is_offset(e2);
                    }
                    if (is_minus_one(e2)) {
                        return is_offset(e1);
                    }
                }
            }
            return !is_arith_expr(e);
        }

        bool is_minus_one(expr const * e) const { 
            rational r; return a.is_numeral(e, r) && r.is_minus_one(); 
        }

        bool test_ineq(expr* e) const {
            SASSERT(a.is_le(e) || a.is_ge(e) || m.is_eq(e));
            SASSERT(to_app(e)->get_num_args() == 2);
            expr * lhs = to_app(e)->get_arg(0);
            expr * rhs = to_app(e)->get_arg(1);
            if (is_offset(lhs) && is_offset(rhs)) 
                return true;    
            if (!is_numeric(rhs)) 
                std::swap(lhs, rhs);
            if (!is_numeric(rhs)) 
                return false;    
            // lhs can be 'x' or '(+ x (* -1 y))'
            if (is_offset(lhs))
                return true;
            expr* arg1, *arg2;
            if (!a.is_add(lhs, arg1, arg2)) 
                return false;    
            // x
            if (m_test_for_utvpi) {
                return is_offset(arg1) && is_offset(arg2);
            }
            if (is_arith_expr(arg1)) 
                std::swap(arg1, arg2);
            if (is_arith_expr(arg1))
                return false;
            // arg2: (* -1 y)
            expr* m1, *m2;
            if (!a.is_mul(arg2, m1, m2))
                return false;
            return is_minus_one(m1) && is_offset(m2);
        }

        bool test_eq(expr* e) const {
            expr* lhs, *rhs;
            VERIFY(m.is_eq(e, lhs, rhs));
            if (!a.is_int_real(lhs)) {
                return true;
            }
            if (a.is_numeral(lhs) || a.is_numeral(rhs)) {
                return test_ineq(e);
            }
            return 
                test_term(lhs) && 
                test_term(rhs) &&
                !a.is_mul(lhs) &&
                !a.is_mul(rhs);
        }

        bool test_term(expr* e) const {
            if (m.is_bool(e)) {
                return true;
            }
            if (a.is_numeral(e)) {
                return true;
            }
            if (is_offset(e)) {
                return true;
            }
            expr* lhs, *rhs;
            if (a.is_add(e, lhs, rhs)) {
                if (!a.is_numeral(lhs)) {
                    std::swap(lhs, rhs);
                }
                return a.is_numeral(lhs) && is_offset(rhs);
            }
            if (a.is_mul(e, lhs, rhs)) {
                return is_minus_one(lhs) || is_minus_one(rhs);
            }
            return false;
        }

        bool is_non_arith_or_basic(expr* e) {
            if (!is_app(e)) {
                return false;
            }
            family_id fid = to_app(e)->get_family_id();

            if (fid == null_family_id && 
                !m.is_bool(e) && 
                to_app(e)->get_num_args() > 0) {
                return true;
            }
            return 
                fid != m.get_basic_family_id() &&
                fid != null_family_id &&
                fid != a.get_family_id() &&
                fid != bv.get_family_id();
        }

    public:
        test_diff_logic(ast_manager& m): m(m), a(m), bv(m), m_is_dl(true), m_test_for_utvpi(false) {}
       
        void test_for_utvpi() { m_test_for_utvpi = true; }

        void operator()(expr* e) {
            if (!m_is_dl) {
                return;
            }
            if (a.is_le(e) || a.is_ge(e)) {
                m_is_dl = test_ineq(e);
            }
            else if (m.is_eq(e)) {
                m_is_dl = test_eq(e);
            }
            else if (is_non_arith_or_basic(e)) {
                m_is_dl = false;
            }
            else if (is_app(e)) {
                app* a = to_app(e);                
                for (unsigned i = 0; m_is_dl && i < a->get_num_args(); ++i) {
                    m_is_dl = test_term(a->get_arg(i));
                }
            }

            if (!m_is_dl) {
                char const* msg = "non-diff: ";
                if (m_test_for_utvpi) {
                    msg = "non-utvpi: ";
                }
                IF_VERBOSE(1, verbose_stream() << msg << mk_pp(e, m) << "\n";);
            }
        }

        bool is_dl() const { return m_is_dl; }
    };

    bool is_difference_logic(ast_manager& m, unsigned num_fmls, expr* const* fmls) {
        test_diff_logic test(m);
        expr_fast_mark1 mark;
        for (unsigned i = 0; i < num_fmls; ++i) {
            quick_for_each_expr(test, mark, fmls[i]);
        } 
        return test.is_dl();
    }  

    bool is_utvpi_logic(ast_manager& m, unsigned num_fmls, expr* const* fmls) {
        test_diff_logic test(m);
        test.test_for_utvpi();
        expr_fast_mark1 mark;
        for (unsigned i = 0; i < num_fmls; ++i) {
            quick_for_each_expr(test, mark, fmls[i]);
        } 
        return test.is_dl();
    }  

    void qe_project (ast_manager& m, app_ref_vector& vars, expr_ref& fml, model_ref& M, expr_map& map) {
        th_rewriter rw (m);
        // qe-lite; TODO: use qe_lite aggressively
        qe_lite qe (m);
        qe (vars, fml);
        rw (fml);

        TRACE ("spacer",
                tout << "After qe_lite:\n";
                tout << mk_pp (fml, m) << "\n";
                tout << "Vars:\n";
                for (unsigned i = 0; i < vars.size(); ++i) {
                    tout << mk_pp(vars.get (i), m) << "\n";
                }
              );

        // substitute model values for booleans and
        // use LW projection for arithmetic variables
        if (!vars.empty ()) {
            app_ref_vector arith_vars (m);
            expr_substitution sub (m);
            proof_ref pr (m.mk_asserted (m.mk_true ()), m);
            expr_ref bval (m);
            for (unsigned i = 0; i < vars.size (); i++) {
                if (m.is_bool (vars.get (i))) {
                    // obtain the interpretation of the ith var using model completion
                    VERIFY (M->eval (vars.get (i), bval, true));
                    sub.insert (vars.get (i), bval, pr);
                }
                else {
                    arith_vars.push_back (vars.get (i));
                }
            }
            if (!sub.empty ()) {
                scoped_ptr<expr_replacer> rep = mk_expr_simp_replacer (m);
                rep->set_substitution (&sub);
                (*rep)(fml);
                rw (fml);
                TRACE ("spacer",
                        tout << "Projected Boolean vars:\n" << mk_pp (fml, m) << "\n";
                      );
            }
            // model based projection
            if (!arith_vars.empty ()) {
                TRACE ("spacer",
                        tout << "Arith vars:\n";
                        for (unsigned i = 0; i < arith_vars.size (); ++i) {
                        tout << mk_pp (arith_vars.get (i), m) << "\n";
                        }
                      );
                {
                    scoped_no_proof _sp (m);
                    qe::arith_project (*M, arith_vars, fml, map);
                }
                SASSERT (arith_vars.empty ());
                TRACE ("spacer",
                        tout << "Projected arith vars:\n" << mk_pp (fml, m) << "\n";
                      );
            }
            SASSERT (M->eval (fml, bval, true) && m.is_true (bval)); // M |= fml
            vars.reset ();
            vars.append (arith_vars);
        }
    }

    void subst_vars (ast_manager& m, app_ref_vector const& vars, model_ref& M, expr_ref& fml) {
        expr_safe_replace sub (m);
        model_evaluator mev (m);
        for (unsigned i = 0; i < vars.size (); i++) {
            app* v = vars.get (i);
            expr_ref val (m);
            val = mev.eval (M, v);
            sub.insert (v, val);
        }
        sub (fml);
    }

    /*
     * eliminate simple equalities using qe_lite
     * then, MBP for Booleans (substitute), reals (based on LW), ints (based on Cooper), and arrays
     */
    void qe_project (ast_manager& m, app_ref_vector& vars, expr_ref& fml, model_ref& M, bool reduce_all_selects) {
        th_rewriter rw (m);
        TRACE ("spacer",
                tout << "Before projection:\n";
                tout << mk_pp (fml, m) << "\n";
                tout << "Vars:\n";
                for (unsigned i = 0; i < vars.size(); ++i) {
                    tout << mk_pp(vars.get (i), m) << "\n";
                }
              );

        app_ref_vector arith_vars (m);
        app_ref_vector array_vars (m);
        array_util arr_u (m);
        arith_util ari_u (m);
        expr_safe_replace bool_sub (m);
        expr_ref bval (m);

        while (true) {
            // qe-lite; TODO: use qe_lite aggressively
            qe_lite qe (m);
            qe (vars, fml);
            rw (fml); // TODO: is this okay?!

            TRACE ("spacer",
                    tout << "After qe_lite:\n";
                    tout << mk_pp (fml, m) << "\n";
                    tout << "Vars:\n";
                    for (unsigned i = 0; i < vars.size(); ++i) {
                        tout << mk_pp(vars.get (i), m) << "\n";
                    }
                  );

            bool has_bool_vars = false;

            // sort out vars into bools, arith (int/real), and arrays
            for (unsigned i = 0; i < vars.size (); i++) {
                if (m.is_bool (vars.get (i))) {
                    // obtain the interpretation of the ith var using model completion
                    VERIFY (M->eval (vars.get (i), bval, true));
                    bool_sub.insert (vars.get (i), bval);
                    has_bool_vars = true;
                }
                else if (arr_u.is_array (vars.get (i))) {
                    array_vars.push_back (vars.get (i));
                }
                else {
                    SASSERT (ari_u.is_int (vars.get (i)) || ari_u.is_real (vars.get (i)));
                    arith_vars.push_back (vars.get (i));
                }
            }

            // substitute Booleans
            if (has_bool_vars) {
                bool_sub (fml);
                //rw (fml);
                TRACE ("spacer",
                        tout << "Projected Booleans:\n" << mk_pp (fml, m) << "\n";
                      );
                bool_sub.reset ();
            }

            TRACE ("spacer",
                    tout << "Array vars:\n";
                    for (unsigned i = 0; i < array_vars.size (); ++i) {
                        tout << mk_pp (array_vars.get (i), m) << "\n";
                    }
                  );

            vars.reset ();

            // project arrays
            {
                scoped_no_proof _sp (m);
                qe::array_project (*M, array_vars, fml, vars, reduce_all_selects);
                SASSERT (array_vars.empty ());
            }

            TRACE ("spacer",
                    tout << "extended model:\n";
                    model_pp (tout, *M);
                    tout << "Auxiliary variables of index and value sorts:\n";
                    for (unsigned i = 0; i < vars.size (); i++) {
                        tout << mk_pp (vars.get (i), m) << "\n";
                    }
                  );

            if (vars.empty ()) break;
        }

        // project reals and ints
        if (!arith_vars.empty ()) {
            TRACE ("spacer",
                    tout << "Arith vars:\n";
                    for (unsigned i = 0; i < arith_vars.size (); ++i) {
                    tout << mk_pp (arith_vars.get (i), m) << "\n";
                    }
                  );
            {
                scoped_no_proof _sp (m);
                qe::arith_project (*M, arith_vars, fml);
            }

            TRACE ("spacer",
                    tout << "Projected arith vars:\n" << mk_pp (fml, m) << "\n";
                    tout << "Remaining arith vars:\n";
                    for (unsigned i = 0; i < arith_vars.size (); i++) {
                        tout << mk_pp (arith_vars.get (i), m) << "\n";
                    }
                  );
        }

        // substitute any remaining arith vars
        if (!arith_vars.empty ()) {
            subst_vars (m, arith_vars, M, fml);
            TRACE ("spacer",
                    tout << "After substituting remaining arith vars:\n";
                    tout << mk_pp (fml, m) << "\n";
                  );
        }

        DEBUG_CODE (
            model_evaluator mev (m);
            mev.eval_heavy (M, fml, bval);
            SASSERT (m.is_true (bval));
        );

        vars.reset ();
    }
  
    static expr* apply_accessor(ast_manager &m,
                                ptr_vector<func_decl> const& acc,
                                unsigned j,
                                func_decl* f,
                                expr* c) {
        if (is_app(c) && to_app(c)->get_decl() == f) {
            return to_app(c)->get_arg(j);
        }
        else {
            return m.mk_app(acc[j], c);
        }
    }

    void expand_literals(ast_manager &m, expr_ref_vector& conjs) {
        if (conjs.empty ()) return;
        arith_util arith(m);
        datatype_util dt(m);
        bv_util       bv(m);
        expr* e1, *e2, *c, *val;
        rational r;
        unsigned bv_size;

        TRACE("spacer", 
                tout << "begin expand\n";
                for (unsigned i = 0; i < conjs.size(); ++i) {
                    tout << mk_pp(conjs[i].get(), m) << "\n";
                });

        for (unsigned i = 0; i < conjs.size(); ++i) {
            expr* e = conjs[i].get();
            if (m.is_eq(e, e1, e2) && arith.is_int_real(e1)) {
                conjs[i] = arith.mk_le(e1,e2);
                if (i+1 == conjs.size()) {
                    conjs.push_back(arith.mk_ge(e1, e2));
                }
                else {
                    conjs.push_back(conjs[i+1].get());
                    conjs[i+1] = arith.mk_ge(e1, e2);
                }
                ++i;
            }
            else if ((m.is_eq(e, c, val) && is_app(val) && dt.is_constructor(to_app(val))) ||
                    (m.is_eq(e, val, c) && is_app(val) && dt.is_constructor(to_app(val)))){
                func_decl* f = to_app(val)->get_decl();
                func_decl* r = dt.get_constructor_recognizer(f);
                conjs[i] = m.mk_app(r, c);
                ptr_vector<func_decl> const& acc = *dt.get_constructor_accessors(f);
                for (unsigned j = 0; j < acc.size(); ++j) {
                    conjs.push_back(m.mk_eq(apply_accessor(m, acc, j, f, c), to_app(val)->get_arg(j)));
                }
            }
            else if ((m.is_eq(e, c, val) && bv.is_numeral(val, r, bv_size)) ||
                    (m.is_eq(e, val, c) && bv.is_numeral(val, r, bv_size))) {
                rational two(2);
                for (unsigned j = 0; j < bv_size; ++j) {
                    parameter p(j);
                    //expr* e = m.mk_app(bv.get_family_id(), OP_BIT2BOOL, 1, &p, 1, &c);
                    expr* e = m.mk_eq(m.mk_app(bv.get_family_id(), OP_BIT1), bv.mk_extract(j, j, c));
                    if ((r % two).is_zero()) {
                        e = m.mk_not(e);
                    }
                    r = div(r, two);
                    if (j == 0) {
                        conjs[i] = e;
                    }
                    else {
                        conjs.push_back(e);
                    }
                }
            }
        }
        TRACE("spacer", 
                tout << "end expand\n";
                for (unsigned i = 0; i < conjs.size(); ++i) {
                    tout << mk_pp(conjs[i].get(), m) << "\n";
                });
    }

}

template class rewriter_tpl<spacer::ite_hoister_cfg>;



