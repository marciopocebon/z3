/*
  Copyright (c) 2017 Microsoft Corporation
  Author: Lev Nachmanson
*/

#pragma once
#include "util/lp/lar_solver.h"
#include "util/lp/nra_solver.h"
#include "nlsat/nlsat_solver.h"
#include "math/polynomial/polynomial.h"
#include "math/polynomial/algebraic_numbers.h"
#include "util/map.h"


namespace nra {

    struct solver::imp {
        lean::lar_solver& s;
        reslimit&      m_limit;  // TBD: extract from lar_solver
        params_ref     m_params; // TBD: pass from outside
        u_map<polynomial::var> m_lp2nl;  // map from lar_solver variables to nlsat::solver variables
        nlsat::solver m_nlsat;

        struct mon_eq {
            mon_eq(lean::var_index v, unsigned sz, lean::var_index const* vs):
                m_v(v), m_vs(sz, vs) {}
            lean::var_index          m_v;
            svector<lean::var_index> m_vs;
        };

        vector<mon_eq>                   m_monomials;
        unsigned_vector                  m_lim;
        mutable std::unordered_map<lean::var_index, rational> m_variable_values; // current model

        imp(lean::lar_solver& s, reslimit& lim, params_ref const& p): 
            s(s), 
            m_limit(lim),
            m_params(p),
            m_nlsat(m_limit, m_params) {
        }

        bool need_check() {
            return !m_monomials.empty() && !check_assignments();
        }

        void add(lean::var_index v, unsigned sz, lean::var_index const* vs) {
            m_monomials.push_back(mon_eq(v, sz, vs));
        }

        void push() {
            m_lim.push_back(m_monomials.size());
        }

        void pop(unsigned n) {
            if (n == 0) return;
            m_monomials.shrink(m_lim[m_lim.size() - n]);
            m_lim.shrink(m_lim.size() - n);       
        }

        /*
          \brief Check if polynomials are well defined.
          multiply values for vs and check if they are equal to value for v.
          epsilon has been computed.
        */
        bool check_assignment(mon_eq const& m) const {
            rational r1 = m_variable_values[m.m_v];
            rational r2(1);
            for (auto w : m.m_vs) {
                r2 *= m_variable_values[w];
            }
            return r1 == r2;
        }

        bool check_assignments() const {
            s.get_model(m_variable_values);
            for (auto const& m : m_monomials) {
                if (!check_assignment(m)) return false;
            }
            return true;
        }


        /**
           \brief one-shot nlsat check.
           A one shot checker is the least functionality that can 
           enable non-linear reasoning. 
           In addition to checking satisfiability we would also need
           to identify equalities in the model that should be assumed
           with the remaining solver.
           
           TBD: use partial model from lra_solver to prime the state of nlsat_solver.
        */
        lbool check(lean::explanation_t& ex) {
            SASSERT(need_check());
            m_nlsat.reset();
            m_lp2nl.reset();
            vector<nlsat::assumption, false> core;

            // add linear inequalities from lra_solver
            for (unsigned i = 0; i < s.constraint_count(); ++i) {
                add_constraint(i);
            }

            // add polynomial definitions.
            for (auto const& m : m_monomials) {
                add_monomial_eq(m);
            }
            // TBD: add variable bounds?

            lbool r = m_nlsat.check(); 
            TRACE("arith", m_nlsat.display(tout << r << "\n"););
            switch (r) {
            case l_true: 
                break;
            case l_false: 
                ex.reset();
                m_nlsat.get_core(core);
                for (auto c : core) {
                    unsigned idx = static_cast<unsigned>(static_cast<imp*>(c) - this);
                    ex.push_back(std::pair<rational, unsigned>(rational(1), idx));
                    TRACE("arith", tout << "ex: " << idx << "\n";);
                }
                break;

            case l_undef:
                break;
            }            
            return r;
        }                

        void add_monomial_eq(mon_eq const& m) {
            polynomial::manager& pm = m_nlsat.pm();
            svector<polynomial::var> vars;
            for (auto v : m.m_vs) {
                vars.push_back(lp2nl(v));
            }
            polynomial::monomial_ref m1(pm.mk_monomial(vars.size(), vars.c_ptr()), pm);
            polynomial::monomial_ref m2(pm.mk_monomial(lp2nl(m.m_v), 1), pm);
            polynomial::monomial* mls[2] = { m1, m2 };
            polynomial::scoped_numeral_vector coeffs(pm.m());
            coeffs.push_back(mpz(1));
            coeffs.push_back(mpz(-1));
            polynomial::polynomial_ref p(pm.mk_polynomial(2, coeffs.c_ptr(), mls),  pm);
            polynomial::polynomial* ps[1] = { p };
            bool even[1] = { false };
            nlsat::literal lit = m_nlsat.mk_ineq_literal(nlsat::atom::kind::EQ, 1, ps, even);
            m_nlsat.mk_clause(1, &lit, 0);
        }

        void add_constraint(unsigned idx) {
            auto& c = s.get_constraint(idx);
            auto& pm = m_nlsat.pm();
            auto k = c.m_kind;
            auto rhs = c.m_right_side;
            auto lhs = c.get_left_side_coefficients();
            auto sz = lhs.size();
            svector<polynomial::var> vars;
            rational den = denominator(rhs);
            for (auto kv : lhs) {
                vars.push_back(lp2nl(kv.second));
                den = lcm(den, denominator(kv.first));
            }
            vector<rational> coeffs;
            for (auto kv : lhs) {
                coeffs.push_back(den * kv.first);
            }
            rhs *= den;
            polynomial::polynomial_ref p(pm.mk_linear(sz, coeffs.c_ptr(), vars.c_ptr(), -rhs), pm);
            polynomial::polynomial* ps[1] = { p };
            bool is_even[1] = { false };
            nlsat::literal lit;
            switch (k) {
            case lean::lconstraint_kind::LE:
                lit = ~m_nlsat.mk_ineq_literal(nlsat::atom::kind::GT, 1, ps, is_even);
                break;
            case lean::lconstraint_kind::GE:
                lit = ~m_nlsat.mk_ineq_literal(nlsat::atom::kind::LT, 1, ps, is_even);
                break;
            case lean::lconstraint_kind::LT:
                lit = m_nlsat.mk_ineq_literal(nlsat::atom::kind::LT, 1, ps, is_even);
                break;
            case lean::lconstraint_kind::GT:
                lit = m_nlsat.mk_ineq_literal(nlsat::atom::kind::GT, 1, ps, is_even);
                break;
            case lean::lconstraint_kind::EQ:
                lit = m_nlsat.mk_ineq_literal(nlsat::atom::kind::EQ, 1, ps, is_even);
                break;
            }

            nlsat::assumption a = this + idx;
            m_nlsat.mk_clause(1, &lit, a);
        }               

        bool is_int(lean::var_index v) {
            // TBD: is it s.column_is_integer(v), if then the function should take a var_index and not unsigned; s.is_int(v);
            return false;
        }


        polynomial::var lp2nl(lean::var_index v) {
            polynomial::var r;
            if (!m_lp2nl.find(v, r)) {
                r = m_nlsat.mk_var(is_int(v));
                m_lp2nl.insert(v, r);
            }
            return r;
        }

        nlsat::anum const& value(lean::var_index v) const {
           return m_nlsat.value(m_lp2nl.find(v));
        }

        std::ostream& display(std::ostream& out) const {
            for (auto m : m_monomials) {
                out << "v" << m.m_v << " = ";
                for (auto v : m.m_vs) {
                    out << "v" << v << " ";
                }
                out << "\n";
            }
            return out;
        }

    };

    solver::solver(lean::lar_solver& s, reslimit& lim, params_ref const& p) {
        m_imp = alloc(imp, s, lim, p);
    }

    solver::~solver() {
        dealloc(m_imp);
    }

    void solver::add_monomial(lean::var_index v, unsigned sz, lean::var_index const* vs) {
        m_imp->add(v, sz, vs);
    }

    lbool solver::check(lean::explanation_t& ex) {
        return m_imp->check(ex);
    }

    bool solver::need_check() {
        return m_imp->need_check();
    }

    void solver::push() {
        m_imp->push();
    }

    void solver::pop(unsigned n) {
        m_imp->pop(n);
    }

    std::ostream& solver::display(std::ostream& out) const {
        return m_imp->display(out);
    }

    nlsat::anum const& solver::value(lean::var_index v) const {
        return m_imp->value(v);
    }

}