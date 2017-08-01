/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    pdr_dl_interface.h

Abstract:

    SMT2 interface for the datalog PDR

Author:

    Krystof Hoder (t-khoder) 2011-9-22.

Revision History:

--*/

#ifndef PDR_DL_INTERFACE_H_
#define PDR_DL_INTERFACE_H_

#include "util/lbool.h"
#include "muz/base/dl_rule.h"
#include "muz/base/dl_rule_set.h"
#include "muz/base/dl_util.h"
#include "muz/base/dl_engine_base.h"
#include "util/statistics.h"

namespace datalog {
    class context;
}

namespace pdr {

    class context;

    class dl_interface : public datalog::engine_base {
        datalog::context& m_ctx;
        datalog::rule_set m_pdr_rules;
        datalog::rule_set m_old_rules;
        context*          m_context;
        obj_map<func_decl, func_decl*> m_pred2slice;
        ast_ref_vector    m_refs;

        void check_reset();

    public:
        dl_interface(datalog::context& ctx); 
        ~dl_interface();
        
        virtual lbool query(expr* query);

        virtual lbool query_from_lvl (expr* query, unsigned lvl);

        virtual void display_certificate(std::ostream& out) const;

        virtual void collect_statistics(statistics& st) const;

        virtual void reset_statistics();

        virtual expr_ref get_answer();

        virtual expr_ref get_ground_sat_answer ();

        virtual void get_rules_along_trace (datalog::rule_ref_vector& rules);

        virtual unsigned get_num_levels(func_decl* pred);

        virtual expr_ref get_cover_delta(int level, func_decl* pred);
       
        virtual void add_cover(int level, func_decl* pred, expr* property);
               
        virtual void updt_params();

        virtual model_ref get_model();

        virtual proof_ref get_proof();
        
    };
}


#endif
