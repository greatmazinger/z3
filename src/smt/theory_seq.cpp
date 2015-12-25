/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    theory_seq.h

Abstract:

    Native theory solver for sequences.

Author:

    Nikolaj Bjorner (nbjorner) 2015-6-12

Revision History:

--*/

#include "value_factory.h"
#include "smt_context.h"
#include "smt_model_generator.h"
#include "theory_seq.h"
#include "seq_rewriter.h"
#include "ast_trail.h"

using namespace smt;

struct display_expr {
    ast_manager& m;
    display_expr(ast_manager& m): m(m) {}
    std::ostream& display(std::ostream& out, expr* e) const {
        return out << mk_pp(e, m);
    }
};


re2automaton::re2automaton(ast_manager& m): m(m), u(m) {}

eautomaton* re2automaton::re2aut(expr* e) {
    SASSERT(u.is_re(e));
    expr* e1, *e2;
    scoped_ptr<eautomaton> a, b;
    if (u.re.is_to_re(e, e1)) {
        return seq2aut(e1);
    }
    else if (u.re.is_concat(e, e1, e2) && (a = re2aut(e1)) && (b = re2aut(e2))) {
        return eautomaton::mk_concat(*a, *b);
    }
    else if (u.re.is_union(e, e1, e2) && (a = re2aut(e1)) && (b = re2aut(e2))) {
        return eautomaton::mk_union(*a, *b);
    }
    else if (u.re.is_star(e, e1) && (a = re2aut(e1))) {
        a->add_final_to_init_moves();
        a->add_init_to_final();
        return a.detach();            
    }
    else if (u.re.is_plus(e, e1) && (a = re2aut(e1))) {
        a->add_final_to_init_moves();
        return a.detach();            
    }
    else if (u.re.is_opt(e, e1) && (a = re2aut(e1))) {
        a = eautomaton::mk_opt(*a);
        return a.detach();                    
    }
    else if (u.re.is_range(e)) {

    }
    else if (u.re.is_loop(e)) {

    }
#if 0
    else if (u.re.is_intersect(e, e1, e2)) {

    }
    else if (u.re.is_empty(e)) {

    }
#endif
    
    return 0;
}

eautomaton* re2automaton::seq2aut(expr* e) {
    SASSERT(u.is_seq(e));
    zstring s;
    expr* e1, *e2;
    scoped_ptr<eautomaton> a, b;
    if (u.str.is_concat(e, e1, e2) && (a = seq2aut(e1)) && (b = seq2aut(e2))) {
        return eautomaton::mk_concat(*a, *b);
    }
    else if (u.str.is_unit(e, e1)) {
        return alloc(eautomaton, m, e1);
    }
    else if (u.str.is_empty(e)) {
        return eautomaton::mk_epsilon(m);
    }
    else if (u.str.is_string(e, s)) {        
        unsigned init = 0;
        eautomaton::moves mvs;        
        unsigned_vector final;
        final.push_back(s.length());
        for (unsigned k = 0; k < s.length(); ++k) {
            // reference count?
            mvs.push_back(eautomaton::move(m, k, k+1, u.str.mk_char(s, k)));
        }
        return alloc(eautomaton, m, init, final, mvs);
    }
    return 0;
}

void theory_seq::solution_map::update(expr* e, expr* r, enode_pair_dependency* d) {
    m_cache.reset();
    std::pair<expr*, enode_pair_dependency*> value;
    if (m_map.find(e, value)) {
        add_trail(DEL, e, value.first, value.second);
    }
    value.first = r;
    value.second = d;
    m_map.insert(e, value);
    add_trail(INS, e, r, d);
}

void theory_seq::solution_map::add_trail(map_update op, expr* l, expr* r, enode_pair_dependency* d) {
    m_updates.push_back(op);
    m_lhs.push_back(l);
    m_rhs.push_back(r);
    m_deps.push_back(d);
}

expr* theory_seq::solution_map::find(expr* e, enode_pair_dependency*& d) {
    std::pair<expr*, enode_pair_dependency*> value;
    d = 0;
    expr* result = e;
    while (m_map.find(result, value)) {
        d = m_dm.mk_join(d, value.second);
        result = value.first;
    }
    return result;
}

void theory_seq::solution_map::pop_scope(unsigned num_scopes) {
    if (num_scopes == 0) return;    
    m_cache.reset();
    unsigned start = m_limit[m_limit.size() - num_scopes];
    for (unsigned i = m_updates.size(); i > start; ) {
        --i;
        if (m_updates[i] == INS) {
            m_map.remove(m_lhs[i].get());
        }
        else {
            m_map.insert(m_lhs[i].get(), std::make_pair(m_rhs[i].get(), m_deps[i]));
        }
    }
    m_updates.resize(start);
    m_lhs.resize(start);
    m_rhs.resize(start);
    m_deps.resize(start);
    m_limit.resize(m_limit.size() - num_scopes);
}

void theory_seq::solution_map::display(std::ostream& out) const {
    eqdep_map_t::iterator it = m_map.begin(), end = m_map.end();
    for (; it != end; ++it) {
        out << mk_pp(it->m_key, m) << " |-> " << mk_pp(it->m_value.first, m) << "\n";
    }
}

bool theory_seq::exclusion_table::contains(expr* e, expr* r) const {
    if (e->get_id() > r->get_id()) {
        std::swap(e, r);
    }
    return m_table.contains(std::make_pair(e, r));
}

void theory_seq::exclusion_table::update(expr* e, expr* r) {
    if (e->get_id() > r->get_id()) {
        std::swap(e, r);
    }
    if (e != r && !m_table.contains(std::make_pair(e, r))) {
        m_lhs.push_back(e);
        m_rhs.push_back(r);
        m_table.insert(std::make_pair(e, r));
    }
}

void theory_seq::exclusion_table::pop_scope(unsigned num_scopes) {
    if (num_scopes == 0) return;
    unsigned start = m_limit[m_limit.size() - num_scopes];
    for (unsigned i = start; i < m_lhs.size(); ++i) {
        m_table.erase(std::make_pair(m_lhs[i].get(), m_rhs[i].get()));
    }
    m_lhs.resize(start);
    m_rhs.resize(start);
    m_limit.resize(m_limit.size() - num_scopes);
}

void theory_seq::exclusion_table::display(std::ostream& out) const {
    table_t::iterator it = m_table.begin(), end = m_table.end();
    for (; it != end; ++it) {
        out << mk_pp(it->first, m) << " != " << mk_pp(it->second, m) << "\n";
    }
}

theory_seq::theory_seq(ast_manager& m):
    theory(m.mk_family_id("seq")), 
    m(m),
    m_rep(m, m_dm),
    m_factory(0),
    m_ineqs(m),
    m_exclude(m),
    m_axioms(m),
    m_axioms_head(0),
    m_branch_variable_head(0),
    m_incomplete(false), 
    m_has_length(false),
    m_model_completion(false),
    m_mg(0),
    m_rewrite(m), 
    m_util(m),
    m_autil(m),
    m_trail_stack(*this),
    m_accepts_qhead(0),
    m_rejects_qhead(0),
    m_steps_qhead(0) {    
    m_prefix_sym = "seq.prefix.suffix";
    m_suffix_sym = "seq.suffix.prefix";
    m_left_sym = "seq.left";
    m_right_sym = "seq.right";
    m_contains_left_sym = "seq.contains.left";
    m_contains_right_sym = "seq.contains.right";
    m_accept_sym = "aut.accept";
}

theory_seq::~theory_seq() {
    m_trail_stack.reset();
}


final_check_status theory_seq::final_check_eh() { 
    context & ctx   = get_context();
    TRACE("seq", display(tout););
    if (!check_ineqs()) {
        return FC_CONTINUE;
    }
    if (simplify_and_solve_eqs()) {
        return FC_CONTINUE;
    }
    if (solve_nqs()) {
        return FC_CONTINUE;
    }
    if (ctx.inconsistent()) {
        return FC_CONTINUE;
    }
    if (propagate_automata()) {
        return FC_CONTINUE;
    }
    if (branch_variable()) {
        TRACE("seq", tout << "branch\n";);
        return FC_CONTINUE;
    }
    if (split_variable()) {
        TRACE("seq", tout << "split_variable\n";);
        return FC_CONTINUE;
    }
    if (ctx.inconsistent()) {
        return FC_CONTINUE;
    }
    if (!check_length_coherence()) {
        TRACE("seq", tout << "check_length_coherence\n";);
        return FC_CONTINUE;
    }
    if (!check_length_coherence_tbd()) {
        TRACE("seq", tout << "check_length_coherence\n";);
        return FC_CONTINUE;
    }
    if (is_solved()) {
        TRACE("seq", tout << "is_solved\n";);
        return FC_DONE;
    }

    return FC_GIVEUP;
}

bool theory_seq::check_ineqs() {
    context & ctx   = get_context();
    for (unsigned i = 0; i < m_ineqs.size(); ++i) {
        expr* a = m_ineqs[i].get();
        enode_pair_dependency* eqs = 0;
        expr_ref b = canonize(a, eqs);
        if (m.is_true(b)) {
            TRACE("seq", tout << "Evaluates to false: " << mk_pp(a,m) << "\n";);
            ctx.internalize(a, false);
            propagate_lit(eqs, ctx.get_literal(a));
            return false;
        }
        else if (!m.is_false(b)) {
            TRACE("seq", tout << "Disequality is undetermined: " << mk_pp(a, m) << " " << b << "\n";);
        }
    }
    return true;
}

bool theory_seq::branch_variable() {
    context& ctx = get_context();
    unsigned sz = m_eqs.size();
    expr_ref_vector ls(m), rs(m);
    for (unsigned i = 0; i < sz; ++i) {
        unsigned k = (i + m_branch_variable_head) % sz;
        eq e = m_eqs[k];
        TRACE("seq", tout << e.m_lhs << " = " << e.m_rhs << "\n";);
        ls.reset(); rs.reset();
        m_util.str.get_concat(e.m_lhs, ls);
        m_util.str.get_concat(e.m_rhs, rs);
        
        if (!ls.empty() && find_branch_candidate(ls[0].get(), rs)) {
            m_branch_variable_head = k;
            return true;
        }
        if (!rs.empty() && find_branch_candidate(rs[0].get(), ls)) {
            m_branch_variable_head = k;
            return true;
        }
    }
    return false;
}

bool theory_seq::find_branch_candidate(expr* l, expr_ref_vector const& rs) {

    TRACE("seq", tout << mk_pp(l, m) << " " 
          << (is_var(l)?"var":"not var") << "\n";);

    if (!is_var(l)) {
        return false;
    }

    expr_ref v0(m), v(m);
    v0 = m_util.str.mk_empty(m.get_sort(l));
    if (assume_equality(l, v0)) {
        return true;
    }
    for (unsigned j = 0; j < rs.size(); ++j) {
        if (occurs(l, rs[j])) {
            return false;
        }
        zstring s;
        if (m_util.str.is_string(rs[j], s)) {
            for (unsigned k = 1; k < s.length(); ++k) {
                v = m_util.str.mk_string(s.extract(0, k));
                if (v0) v = m_util.str.mk_concat(v0, v);
                if (assume_equality(l, v)) {
                    return true;
                }
            }
        }
        v0 = (j == 0)? rs[0] : m_util.str.mk_concat(v0, rs[j]); 
        if (assume_equality(l, v0)) {
            return true;
        }
    }           
    return false;
}

bool theory_seq::assume_equality(expr* l, expr* r) {
    context& ctx = get_context();
    if (m_exclude.contains(l, r)) {
        return false;
    }
    else {
        TRACE("seq", tout << mk_pp(l, m) << " = " << mk_pp(r, m) << "\n";);
        enode* n1 = ensure_enode(l);
        enode* n2 = ensure_enode(r);
        ctx.mark_as_relevant(n1);
        ctx.mark_as_relevant(n2);
        ctx.assume_eq(n1, n2);
        return true;
    }
}

bool theory_seq::split_variable() {

    return false;
}

bool theory_seq::check_length_coherence() {
    if (!m_has_length) return true;
    context& ctx = get_context();
    bool coherent = true;
    for (unsigned i = 0; i < m_eqs.size(); ++i) {
        m_eqs[i].m_dep;
        expr_ref v1(m), v2(m), l(m_eqs[i].m_lhs), r(m_eqs[i].m_rhs);
        expr_ref len1(m_util.str.mk_length(l), m);
        expr_ref len2(m_util.str.mk_length(r), m);
        enode* n1 = ensure_enode(len1);
        enode* n2 = ensure_enode(len2);
        if (n1->get_root() != n2->get_root()) {
            TRACE("seq", tout << len1 << " = " << len2 << "\n";);
            propagate_eq(m_eqs[i].m_dep, n1, n2);
            coherent = false;
        }
    }
    return coherent;
}

bool theory_seq::check_length_coherence_tbd() {
    if (!m_has_length) return true;
    context& ctx = get_context();
    bool coherent = true;
    // each variable that canonizes to itself can have length 0.
    unsigned sz = get_num_vars();
    for (unsigned i = 0; i < sz; ++i) {
        unsigned j = (i + m_branch_variable_head) % sz;
        enode* n = get_enode(j);
        expr*  e = n->get_owner();
        if (m_util.is_re(e)) {
            continue;
        }
        SASSERT(m_util.is_seq(e));
        // extend length of variables.
        enode_pair_dependency* dep = 0;
        expr* f = m_rep.find(e, dep);
        if (is_var(f) && f == e) {
            expr_ref emp(m_util.str.mk_empty(m.get_sort(e)), m);
            TRACE("seq", tout << "Unsolved " << mk_pp(e, m) << "\n";);
            if (!assume_equality(e, emp)) {
                expr_ref head(m), tail(m);
                mk_decompose(e, emp, head, tail);
                // e = emp \/ e = unit(head.elem(e))*tail(e)
                expr_ref conc(m_util.str.mk_concat(head, tail), m);
                add_axiom(mk_eq(e, emp, false), mk_eq(e, conc, false));
                assume_equality(tail, emp);
            }
            m_branch_variable_head = j + 1;
            return false;
        }
    }
    return coherent;
}    

void theory_seq::mk_decompose(expr* e, expr_ref& emp, expr_ref& head, expr_ref& tail) {
    sort* char_sort = 0;
    VERIFY(m_util.is_seq(m.get_sort(e), char_sort));
    tail = mk_skolem(symbol("seq.tail"), e);
    expr_ref v(mk_skolem(symbol("seq.head.elem"), e, 0, 0, char_sort), m);
    head = m_util.str.mk_unit(v);
    emp  = m_util.str.mk_empty(m.get_sort(e));
}

bool theory_seq::check_ineq_coherence() {
    bool all_false = true;
    for (unsigned i = 0; all_false && i < m_ineqs.size(); ++i) {
        expr* a = m_ineqs[i].get();
        enode_pair_dependency* eqs = 0;
        expr_ref b = canonize(a, eqs);
        all_false = m.is_false(b);
        if (all_false) {
            TRACE("seq", tout << "equality is undetermined: " << mk_pp(a, m) << " " << b << "\n";);
        }
    }
    return all_false;
}

/*
   - Eqs = 0
   - Diseqs evaluate to false
   - lengths are coherent.
*/

bool theory_seq::is_solved() {
    if (!m_eqs.empty()) {
        return false;
    }
    if (!check_ineq_coherence()) {
        return false;
    }
    if (!m_re2aut.empty()) {
        return false;
    }
    
    SASSERT(check_length_coherence());

    return true;

}

void theory_seq::propagate_lit(enode_pair_dependency* dep, unsigned n, literal const* lits, literal lit) {
    context& ctx = get_context();
    ctx.mark_as_relevant(lit);
    vector<enode_pair, false> _eqs;
    m_dm.linearize(dep, _eqs);
    TRACE("seq", ctx.display_detailed_literal(tout, lit); 
          tout << " <- "; display_deps(tout, dep);); 
    justification* js = 
        ctx.mk_justification(
            ext_theory_propagation_justification(
                get_id(), ctx.get_region(), n, lits, _eqs.size(), _eqs.c_ptr(), lit));

    ctx.assign(lit, js);
}

void theory_seq::set_conflict(enode_pair_dependency* dep, literal_vector const& lits) {
    context& ctx = get_context();
    vector<enode_pair, false> _eqs;
    m_dm.linearize(dep, _eqs);
    TRACE("seq", ctx.display_literals_verbose(tout, lits.size(), lits.c_ptr()); display_deps(tout, dep); ;); 
    ctx.set_conflict(
        ctx.mk_justification(
            ext_theory_conflict_justification(
                get_id(), ctx.get_region(), lits.size(), lits.c_ptr(), _eqs.size(), _eqs.c_ptr(), 0, 0)));
}

void theory_seq::propagate_eq(enode_pair_dependency* dep, enode* n1, enode* n2) {
    context& ctx = get_context();
    vector<enode_pair, false> _eqs;
    m_dm.linearize(dep, _eqs);
    TRACE("seq",
          tout << mk_pp(n1->get_owner(), m) << " = " << mk_pp(n2->get_owner(), m) << " <- ";
          display_deps(tout, dep);
          ); 
    
    justification* js = ctx.mk_justification(
        ext_theory_eq_propagation_justification(
            get_id(), ctx.get_region(), 0, 0, _eqs.size(), _eqs.c_ptr(), n1, n2));
    ctx.assign_eq(n1, n2, eq_justification(js));
}



bool theory_seq::simplify_eq(expr* l, expr* r, enode_pair_dependency* deps) {
    context& ctx = get_context();
    seq_rewriter rw(m);
    expr_ref_vector lhs(m), rhs(m);
    expr_ref lh = canonize(l, deps);
    expr_ref rh = canonize(r, deps);
    if (!rw.reduce_eq(lh, rh, lhs, rhs)) {
        // equality is inconsistent.
        TRACE("seq", tout << lh << " != " << rh << "\n";);
        set_conflict(deps);
        return true;
    }
    if (unchanged(l, lhs) && unchanged(r, rhs)) {
        return false;
    }
    SASSERT(lhs.size() == rhs.size());
    for (unsigned i = 0; i < lhs.size(); ++i) {
        expr_ref l(lhs[i].get(), m);
        expr_ref r(rhs[i].get(), m);
        if (m_util.is_seq(l) || m_util.is_re(l)) {
            m_eqs.push_back(eq(l, r, deps));
        }
        else {
            propagate_eq(deps, ensure_enode(l), ensure_enode(r));
        }
    }    
    TRACE("seq",
          tout << mk_pp(l, m) << " = " << mk_pp(r, m) << " => ";
          for (unsigned i = 0; i < m_eqs.size(); ++i) {
              tout << m_eqs[i].m_lhs << " = " << m_eqs[i].m_rhs << "; ";
          }
          tout << "\n";
          );
    return true;
}

bool theory_seq::solve_unit_eq(expr* l, expr* r, enode_pair_dependency* deps) {
    expr_ref lh = canonize(l, deps);
    expr_ref rh = canonize(r, deps);
    if (lh == rh) {
        return true;
    }
    if (is_var(lh) && !occurs(lh, rh)) {
        add_solution(lh, rh, deps);
        return true;
    }
    if (is_var(rh) && !occurs(rh, lh)) {
        add_solution(rh, lh, deps);
        return true;
    }
    // Use instead reference counts for dependencies to GC?

    // TBD: Solutions to units are not necessarily variables, but
    // they may induce new equations.

    return false;
}

bool theory_seq::occurs(expr* a, expr* b) {
    // true if a occurs under an interpreted function or under left/right selector.    
    SASSERT(is_var(a));
    expr* e1, *e2;
    while (is_left_select(a, e1) || is_right_select(a, e1)) {
        a = e1;
    }
    if (m_util.str.is_concat(b, e1, e2)) {
        return occurs(a, e1) || occurs(a, e2);
    }
    while (is_left_select(b, e1) || is_right_select(b, e1)) {
        b = e1;
    }          
    if (a == b) {
        return true;
    }  
    return false;
}

bool theory_seq::is_var(expr* a) {
    return 
        m_util.is_seq(a) &&
        !m_util.str.is_concat(a) && 
        !m_util.str.is_empty(a)  && 
        !m_util.str.is_string(a) && 
        !m_util.str.is_unit(a);
}

bool theory_seq::is_left_select(expr* a, expr*& b) {
    return is_skolem(m_left_sym, a) && (b = to_app(a)->get_arg(0), true);
}

bool theory_seq::is_right_select(expr* a, expr*& b) {
    return is_skolem(m_right_sym, a) && (b = to_app(a)->get_arg(0), true);
}

bool theory_seq::is_head_elem(expr* e) const {
    return is_skolem(symbol("seq.head.elem"), e);
}

void theory_seq::add_solution(expr* l, expr* r, enode_pair_dependency* deps) {
    context& ctx = get_context();
    m_rep.update(l, r, deps);
    // TBD: skip new equalities for non-internalized terms.
    if (ctx.e_internalized(l) && ctx.e_internalized(r)) {
        propagate_eq(deps, ctx.get_enode(l), ctx.get_enode(r));
    }
}

bool theory_seq::simplify_eqs() {
    return pre_process_eqs(true);
}

bool theory_seq::solve_basic_eqs() {
    return pre_process_eqs(false);
}

bool theory_seq::pre_process_eqs(bool simplify_or_solve) {
    context& ctx = get_context();
    bool change = false;
    for (unsigned i = 0; !ctx.inconsistent() && i < m_eqs.size(); ++i) {
        eq e = m_eqs[i];
        
        if (simplify_or_solve?
            simplify_eq(e.m_lhs, e.m_rhs, e.m_dep):
            solve_unit_eq(e.m_lhs, e.m_rhs, e.m_dep)) {
            if (i + 1 != m_eqs.size()) {
                eq e1 = m_eqs[m_eqs.size()-1];
                m_eqs.set(i, e1);
                --i;
                ++m_stats.m_num_reductions;
            }
            m_eqs.pop_back();
            change = true;
        }
    }    
    return change;
}

bool theory_seq::solve_nqs() {
    bool change = false;
    context & ctx = get_context();
    for (unsigned i = 0; !ctx.inconsistent() && i < m_nqs.size(); ++i) {
        if (!m_nqs[i].is_solved()) {
            change = solve_ne(i) || change;
        }
    }
    return change;
}

bool theory_seq::solve_ne(unsigned idx) {
    context& ctx = get_context();
    seq_rewriter rw(m);
    bool change = false;
    ne const& n = m_nqs[idx];
    TRACE("seq", display_disequation(tout, n););

    SASSERT(!n.is_solved());
    unsigned num_undef_lits = 0;
    for (unsigned i = 0; i < n.m_lits.size(); ++i) {
        switch (ctx.get_assignment(n.m_lits[i])) {
        case l_false:
            // mark as solved in 
            mark_solved(idx);
            return false;
        case l_true:
            break;            
        case l_undef:
            ++num_undef_lits;
            break;
        }
    }
    for (unsigned i = 0; i < n.m_lhs.size(); ++i) {
        expr_ref_vector lhs(m), rhs(m);
        enode_pair_dependency* deps = 0;
        expr* l = n.m_lhs[i];
        expr* r = n.m_rhs[i];
        expr_ref lh = canonize(l, deps);
        expr_ref rh = canonize(r, deps);
        if (!rw.reduce_eq(lh, rh, lhs, rhs)) {
            mark_solved(idx);
            return change;
        }
        else if (unchanged(l, lhs) && unchanged(r, rhs)) {
            // continue
        }
        else if (unchanged(r, lhs) && unchanged(l, rhs)) {
            // continue
        }
        else {
            TRACE("seq", 
                  for (unsigned j = 0; j < lhs.size(); ++j) {
                      tout << mk_pp(lhs[j].get(), m) << " ";
                  }
                  tout << "\n";
                  tout << mk_pp(l, m) << " != " << mk_pp(r, m) << "\n";);

            for (unsigned j = 0; j < lhs.size(); ++j) {
                expr_ref nl(lhs[j].get(), m);
                expr_ref nr(rhs[j].get(), m);
                if (m_util.is_seq(nl) || m_util.is_re(nl)) {
                    m_trail_stack.push(push_ne(*this, idx, nl, nr));
                }
                else {
                    literal lit(mk_eq(nl, nr, false));
                    m_trail_stack.push(push_lit(*this, idx, lit));
                    ctx.mark_as_relevant(lit);
                    switch (ctx.get_assignment(lit)) {
                    case l_false:
                        mark_solved(idx);
                        return false;
                    case l_true:
                        break;
                    case l_undef:
                        ++num_undef_lits;
                        break;
                    }
                }
            }                
            m_trail_stack.push(push_dep(*this, idx, deps));
            erase_index(idx, i);
            --i;
            change = true;
        }
    }
    if (num_undef_lits == 0 && n.m_lhs.empty()) {
        literal_vector lits(n.m_lits);
        lits.push_back(~mk_eq(n.m_l, n.m_r, false));
        set_conflict(n.m_dep, lits);
        return true;
    }
    return change;
}


void theory_seq::mark_solved(unsigned idx) {
    m_trail_stack.push(solved_ne(*this, idx));
}

void theory_seq::erase_index(unsigned idx, unsigned i) {
    ne const& n = m_nqs[idx];   
    unsigned sz = n.m_lhs.size();
    if (i + 1 != sz) {
        m_trail_stack.push(set_ne(*this, idx, i, n.m_lhs[sz-1], n.m_rhs[sz-1]));
    }
    m_trail_stack.push(pop_ne(*this, idx));
}

bool theory_seq::simplify_and_solve_eqs() {
    context & ctx = get_context();
    bool change = simplify_eqs();
    while (!ctx.inconsistent() && solve_basic_eqs()) {
        simplify_eqs();
        change = true;
    }
    return change;
}

void theory_seq::internalize_eq_eh(app * atom, bool_var v) {
}

bool theory_seq::internalize_atom(app* a, bool) { 
    return internalize_term(a);
}

bool theory_seq::internalize_term(app* term) { 
    TRACE("seq", tout << mk_pp(term, m) << "\n";);
    context & ctx   = get_context();
    unsigned num_args = term->get_num_args();
    for (unsigned i = 0; i < num_args; i++) {
        expr* arg = term->get_arg(i);
        mk_var(ensure_enode(arg));
    }
    if (m.is_bool(term)) {
        bool_var bv = ctx.mk_bool_var(term);
        ctx.set_var_theory(bv, get_id());
        ctx.mark_as_relevant(bv);
        TRACE("seq", tout << mk_pp(term, m) << ": " << bv << "\n";);
    }
    else {
        enode* e = 0;
        if (ctx.e_internalized(term)) {
            e = ctx.get_enode(term);
        }
        else {
            e = ctx.mk_enode(term, false, m.is_bool(term), true);
        } 
        mk_var(e);
    }
    if (m_util.str.is_length(term) && !m_has_length) {
        m_trail_stack.push(value_trail<theory_seq, bool>(m_has_length)); 
        m_has_length = true; 
    }
    if (!m_util.str.is_concat(term) &&
        !m_util.str.is_string(term) &&
        !m_util.str.is_empty(term)  && 
        !m_util.str.is_unit(term)   &&
        !m_util.str.is_suffix(term) &&
        !m_util.str.is_prefix(term) &&
        !m_util.str.is_contains(term) &&
        !m_util.is_skolem(term)) {
        set_incomplete(term);
    }
    return true;
}

void theory_seq::apply_sort_cnstr(enode* n, sort* s) {
    mk_var(n);    
}

void theory_seq::display(std::ostream & out) const {
    if (m_eqs.size() == 0 &&
        m_nqs.size() == 0 && 
        m_ineqs.empty() &&
        m_rep.empty() && 
        m_exclude.empty()) {
        return;
    }
    out << "Theory seq\n";
    if (m_eqs.size() > 0) {
        out << "Equations:\n";
        display_equations(out);
    }
    if (m_nqs.size() > 0) {
        out << "Disequations:\n";
        display_disequations(out);
    }
    if (!m_ineqs.empty()) {
        out << "Negative constraints:\n";
        for (unsigned i = 0; i < m_ineqs.size(); ++i) {
            out << mk_pp(m_ineqs[i], m) << "\n";
        }
    }
    if (!m_re2aut.empty()) {
        out << "Regex\n";
        obj_map<expr, eautomaton*>::iterator it = m_re2aut.begin(), end = m_re2aut.end();
        for (; it != end; ++it) {
            out << mk_pp(it->m_key, m) << "\n";
            display_expr disp(m);
            it->m_value->display(out, disp);
        }
    }
    if (!m_rep.empty()) {
        out << "Solved equations:\n";
        m_rep.display(out);
    }
    if (!m_exclude.empty()) {
        out << "Exclusions:\n";
        m_exclude.display(out);
    }
}

void theory_seq::display_equations(std::ostream& out) const {
    for (unsigned i = 0; i < m_eqs.size(); ++i) {
        eq const& e = m_eqs[i];
        out << e.m_lhs << " = " << e.m_rhs << " <- ";
        display_deps(out, e.m_dep);
    }       
}

void theory_seq::display_disequations(std::ostream& out) const {
    for (unsigned i = 0; i < m_nqs.size(); ++i) {
        display_disequation(out, m_nqs[i]);
    }       
}

void theory_seq::display_disequation(std::ostream& out, ne const& e) const {
    for (unsigned j = 0; j < e.m_lits.size(); ++j) {
        out << e.m_lits[j] << " ";
    }
    if (e.m_lits.size() > 0) {
        out << "\n";
    }
    for (unsigned j = 0; j < e.m_lhs.size(); ++j) {
        out << mk_pp(e.m_lhs[j], m) << " != " << mk_pp(e.m_rhs[j], m) << "\n";
    }
    display_deps(out, e.m_dep);        
}

void theory_seq::display_deps(std::ostream& out, enode_pair_dependency* dep) const {
    vector<enode_pair, false> _eqs;
    const_cast<enode_pair_dependency_manager&>(m_dm).linearize(dep, _eqs);
    for (unsigned i = 0; i < _eqs.size(); ++i) {
        out << " " << mk_pp(_eqs[i].first->get_owner(), m) << " = " << mk_pp(_eqs[i].second->get_owner(), m);
    }
    out << "\n";
}

void theory_seq::collect_statistics(::statistics & st) const {
    st.update("seq num splits", m_stats.m_num_splits);
    st.update("seq num reductions", m_stats.m_num_reductions);
}

void theory_seq::init_model(model_generator & mg) {
    m_factory = alloc(seq_factory, get_manager(), get_family_id(), mg.get_model());
    mg.register_factory(m_factory);
}

model_value_proc * theory_seq::mk_value(enode * n, model_generator & mg) {
    enode_pair_dependency* deps = 0;
    expr_ref e(n->get_owner(), m);
    flet<bool> _model_completion(m_model_completion, true);
    m_rep.reset_cache();
    m_mg = &mg;
    e = canonize(e, deps);
    m_mg = 0;
    SASSERT(is_app(e));
    TRACE("seq", tout << mk_pp(n->get_owner(), m) << " -> " << e << "\n";);
    m_factory->add_trail(e);
    return alloc(expr_wrapper_proc, to_app(e));
}



void theory_seq::set_incomplete(app* term) {
    if (!m_incomplete) { 
        TRACE("seq", tout << "Incomplete operator: " << mk_pp(term, m) << "\n";);
        m_trail_stack.push(value_trail<theory_seq, bool>(m_incomplete)); 
        m_incomplete = true; 
    } 
}

theory_var theory_seq::mk_var(enode* n) {
    if (!m_util.is_seq(n->get_owner()) &&
        !m_util.is_re(n->get_owner())) {
        return null_theory_var;
    }
    if (is_attached_to_var(n)) {
        return n->get_th_var(get_id());
    }
    else {
        theory_var v = theory::mk_var(n);
        get_context().attach_th_var(n, this, v);
        get_context().mark_as_relevant(n);
        return v;
    }
}

bool theory_seq::can_propagate() {
    return m_axioms_head < m_axioms.size();
}

expr_ref theory_seq::canonize(expr* e, enode_pair_dependency*& eqs) {
    expr_ref result = expand(e, eqs);
    m_rewrite(result);
    return result;
}

expr_ref theory_seq::expand(expr* e0, enode_pair_dependency*& eqs) {
    expr_ref result(m);
    enode_pair_dependency* deps = 0;
    expr_dep ed;
    if (m_rep.find_cache(e0, ed)) {
        eqs = m_dm.mk_join(eqs, ed.second);
        result = ed.first;
        TRACE("seq", tout << mk_pp(e0, m) << " |-> " << result << " "; display_deps(tout, eqs););
        return result;
    }
    expr* e = m_rep.find(e0, deps);
    expr* e1, *e2;
    if (m_util.str.is_concat(e, e1, e2)) {
        result = m_util.str.mk_concat(expand(e1, deps), expand(e2, deps));
    }        
    else if (m_util.str.is_empty(e) || m_util.str.is_string(e)) {
        result = e;
    }
    else if (m_util.str.is_prefix(e, e1, e2)) {
        result = m_util.str.mk_prefix(expand(e1, deps), expand(e2, deps));
    }
    else if (m_util.str.is_suffix(e, e1, e2)) {
        result = m_util.str.mk_suffix(expand(e1, deps), expand(e2, deps));
    }
    else if (m_util.str.is_contains(e, e1, e2)) {
        result = m_util.str.mk_contains(expand(e1, deps), expand(e2, deps));
    }
    else if (m_model_completion && is_var(e)) {
        SASSERT(m_factory);
        expr_ref val(m);
        val = m_factory->get_some_value(m.get_sort(e));
        if (val) {
            m_rep.update(e, val, 0);
            result = val;
        }
        else {
            result = e;
        }
    }
    else if (m_model_completion && m_util.str.is_unit(e, e1)) {
        result = expand(e1, deps);
        bv_util bv(m);
        rational val;
        unsigned sz;
        if (bv.is_numeral(result, val, sz) && sz == zstring().num_bits()) {
            svector<bool> val_as_bits;
            for (unsigned i = 0; i < sz; ++i) {
                val_as_bits.push_back(!val.is_even());
                val = div(val, rational(2));
            }
            result = m_util.str.mk_string(zstring(sz, val_as_bits.c_ptr()));
        }
        else {
            result = m_util.str.mk_unit(result);
        }
    }
    else if (m_model_completion && is_head_elem(e)) {
        enode* n = get_context().get_enode(e)->get_root();
        result = n->get_owner();
        if (!m.is_model_value(result)) {
            if (m_util.is_char(result)) {
                result = m_util.str.mk_char('#');
            }
            else {
                result = m_mg->get_some_value(m.get_sort(result));
            }
        }
        m_rep.update(e, result, 0);        
        TRACE("seq", tout << mk_pp(e, m) << " |-> " << result << "\n";);
    }
    else {
        result = e;
    }
    if (result == e0) {
        deps = 0;
    }
    expr_dep edr(result, deps);
    m_rep.add_cache(e0, edr);
    eqs = m_dm.mk_join(eqs, deps);
    TRACE("seq", tout << mk_pp(e0, m) << " |--> " << result << "\n";
          display_deps(tout, eqs););
    return result;
}

void theory_seq::add_dependency(enode_pair_dependency*& dep, enode* a, enode* b) {
    if (a != b) {
        dep = m_dm.mk_join(dep, m_dm.mk_leaf(std::make_pair(a, b)));
    }
}


void theory_seq::propagate() {
    context & ctx = get_context();
    while (m_axioms_head < m_axioms.size() && !ctx.inconsistent()) {
        expr_ref e(m);
        e = m_axioms[m_axioms_head].get();
        deque_axiom(e);
        ++m_axioms_head;
    }
}

void theory_seq::enque_axiom(expr* e) {
    TRACE("seq", tout << "add axioms for: " << mk_pp(e, m) << "\n";);
    m_trail_stack.push(push_back_vector<theory_seq, expr_ref_vector>(m_axioms));
    m_axioms.push_back(e);
}

void theory_seq::deque_axiom(expr* n) {
    if (m_util.str.is_length(n)) {        
        add_length_axiom(n);
    }
    else if (m_util.str.is_index(n)) {
        add_indexof_axiom(n);
    }
    else if (m_util.str.is_replace(n)) {
        add_replace_axiom(n);
    }
    else if (m_util.str.is_extract(n)) {
        add_extract_axiom(n);
    }
    else if (m_util.str.is_at(n)) {
        add_at_axiom(n);
    }
    else if (m_util.str.is_unit(n)) {
        add_length_unit_axiom(n);
    }
    else if (m_util.str.is_empty(n)) {
        add_length_empty_axiom(n);
    }
    else if (m_util.str.is_concat(n)) {
        add_length_concat_axiom(n);
    }
    else if (m_util.str.is_string(n)) {
        add_elim_string_axiom(n);
        // add_length_string_axiom(n);
    }
    else if (m_util.str.is_in_re(n)) {
        add_in_re_axiom(n);
    }
}


/*
  encode that s is not a proper prefix of xs1
  where s1 is all of s, except the last element.
  
  lit or s = "" or s = s1*c
  lit or s = "" or len(c) = 1
  lit or s = "" or !prefix(s, x*s1)
*/
void theory_seq::tightest_prefix(expr* s, expr* x, literal lit1, literal lit2) {
    expr_ref s1 = mk_skolem(symbol("seq.first"), s);
    expr_ref c  = mk_skolem(symbol("seq.last"),  s);
    expr_ref s1c(m_util.str.mk_concat(s1, c), m);
    expr_ref lc(m_util.str.mk_length(c), m);
    expr_ref one(m_autil.mk_int(1), m);
    expr_ref emp(m_util.str.mk_empty(m.get_sort(s)), m);
    literal s_eq_emp = mk_eq(s, emp, false);
    add_axiom(lit1, lit2, s_eq_emp, mk_eq(s, s1c, false));
    add_axiom(lit1, lit2, s_eq_emp, mk_eq(lc, one, false));
    add_axiom(lit1, lit2, s_eq_emp, ~mk_literal(m_util.str.mk_contains(s, m_util.str.mk_concat(x, s1))));
}

/*
  // index of s in t starting at offset.

  let i = Index(t, s, 0):
  
  len(t) = 0  => i = -1
  len(t) != 0 & !contains(t, s) => i = -1
  len(t) != 0 & contains(t, s) => t = xsy & i = len(x)
  len(t) != 0 & contains(t, s) & s != emp => tightest_prefix(x, s)

  let i = Index(t, s, offset)


  0 <= offset < len(t) => xy = t & len(x) = offset & (-1 = indexof(t, s, 0) => -1 = i)
                                                   & (indexof(t, s, 0) >= 0 => indexof(t, s, 0) + offset = i)
  

  offset = len(t) => i = -1
  
  if offset < 0 or offset >= len(t)
     under specified

  optional lemmas:
  (len(s) > len(t)  -> i = -1) 
  (len(s) <= len(t) -> i <= len(t)-len(s))  
*/
void theory_seq::add_indexof_axiom(expr* i) {
    expr* s, *t, *offset;
    rational r;
    VERIFY(m_util.str.is_index(i, t, s, offset));
    expr_ref emp(m), minus_one(m), zero(m), xsy(m);
    minus_one   = m_autil.mk_int(-1);
    zero        = m_autil.mk_int(0);
    emp         = m_util.str.mk_empty(m.get_sort(s));
    literal offset_ne_zero = null_literal;
    bool is_num = m_autil.is_numeral(offset, r); 
    if (is_num && r.is_zero()) {
        offset_ne_zero = null_literal;
    }
    else {
        offset_ne_zero = ~mk_eq(offset, zero, false);
    }
    if (!is_num || r.is_zero()) {
        expr_ref x  = mk_skolem(m_contains_left_sym, t, s);
        expr_ref y  = mk_skolem(m_contains_right_sym, t, s);    
        xsy         = m_util.str.mk_concat(x,s,y);
        literal cnt = mk_literal(m_util.str.mk_contains(t, s));
        literal eq_empty = mk_eq(s, emp, false);
        add_axiom(offset_ne_zero, cnt,  mk_eq(i, minus_one, false));
        add_axiom(offset_ne_zero, ~eq_empty, mk_eq(i, zero, false));
        add_axiom(offset_ne_zero, ~cnt, eq_empty, mk_eq(t, xsy, false));
        tightest_prefix(s, x, ~cnt, offset_ne_zero);
    }
    if (is_num && r.is_zero()) {
        return;
    }
    // offset >= len(t) => indexof(s, t, offset) = -1
    expr_ref len_t(m_util.str.mk_length(t), m);
    literal offset_ge_len = mk_literal(m_autil.mk_ge(mk_sub(offset, len_t), zero));
    add_axiom(offset_ge_len, mk_eq(i, minus_one, false));

    // 0 <= offset & offset < len(t) => t = xy
    // 0 <= offset & offset < len(t) => len(x) = offset
    // 0 <= offset & offset < len(t) & ~contains(s, y) => indexof(t, s, offset) = -1
	// 0 <= offset & offset < len(t) & contains(s, y) => index(t, s, offset) = indexof(y, s, 0) + len(t) 
    expr_ref x = mk_skolem(symbol("seq.indexof.left"), t, s, offset);
    expr_ref y = mk_skolem(symbol("seq.indexof.right"), t, s, offset);
    expr_ref indexof(m_util.str.mk_index(y, s, zero), m);
	// TBD:
    //literal offset_ge_0 = mk_literal(m_autil.mk_ge(offset, zero));
    //add_axiom(~offset_ge_0, offset_ge_len, mk_eq(indexof, i, false));
    //add_axiom(~offset_ge_0, offset_ge_len, mk_eq(m_util.str.mk_length(x), offset, false));
    //add_axiom(~offset_ge_0, offset_ge_len, mk_eq(t, m_util.str.mk_concat(x, y), false));
}

/*
  let r = replace(a, s, t)
  
  (contains(a, s) -> tightest_prefix(s,xs))
  (contains(a, s) -> r = xty & a = xsy) & 
  (!contains(a, s) -> r = a)
   
*/
void theory_seq::add_replace_axiom(expr* r) {
    expr* a, *s, *t;
    VERIFY(m_util.str.is_replace(r, a, s, t));
    expr_ref x  = mk_skolem(m_contains_left_sym, a, s);
    expr_ref y  = mk_skolem(m_contains_right_sym, a, s);    
    expr_ref xty(m_util.str.mk_concat(x, t, y), m);
    expr_ref xsy(m_util.str.mk_concat(x, s, y), m);
    literal cnt = mk_literal(m_util.str.mk_contains(a ,s));
    add_axiom(cnt,  mk_eq(r, a, false));
    add_axiom(~cnt, mk_eq(a, xsy, false));
    add_axiom(~cnt, mk_eq(r, xty, false));
    tightest_prefix(s, x, ~cnt);
}

void theory_seq::add_length_unit_axiom(expr* n) {
    if (!m_has_length) return;
    SASSERT(m_util.str.is_unit(n));
    expr_ref one(m_autil.mk_int(1), m), len(m_util.str.mk_length(n), m);
    add_axiom(mk_eq(len, one, false));
}

void theory_seq::add_length_empty_axiom(expr* n) {
    if (!m_has_length) return;
    SASSERT(m_util.str.is_empty(n));
    expr_ref zero(m_autil.mk_int(0), m), len(m_util.str.mk_length(n), m);
    add_axiom(mk_eq(len, zero, false));
}

void theory_seq::add_elim_string_axiom(expr* n) {
    zstring s;
    VERIFY(m_util.str.is_string(n, s));
    SASSERT(s.length() > 0);
    expr_ref result(m_util.str.mk_unit(m_util.str.mk_char(s, 0)), m);
    for (unsigned i = 1; i < s.length(); ++i) {
        result = m_util.str.mk_concat(result, m_util.str.mk_unit(m_util.str.mk_char(s, i)));
    }
    add_axiom(mk_eq(n, result, false));
    m_rep.update(n, result, 0);
}

void theory_seq::add_length_string_axiom(expr* n) {
    if (!m_has_length) return;
    zstring s;
    VERIFY(m_util.str.is_string(n, s));
    expr_ref len(m_util.str.mk_length(n), m);
    expr_ref ls(m_autil.mk_numeral(rational(s.length(), rational::ui64()), true), m);    
    add_axiom(mk_eq(len, ls, false));
}

void theory_seq::add_length_concat_axiom(expr* n) {
    if (!m_has_length) return;
    expr* a, *b;
    VERIFY(m_util.str.is_concat(n, a, b));
    expr_ref len(m_util.str.mk_length(n), m);
    expr_ref _a(m_util.str.mk_length(a), m);
    expr_ref _b(m_util.str.mk_length(b), m);
    expr_ref ab(m_autil.mk_add(_a, _b), m);
    m_rewrite(ab);
    add_axiom(mk_eq(ab, len, false));
}

/*
    let n = len(x)

    len(x) >= 0
    len(x) = 0 => x = ""
    x = "" => len(x) = 0
 */
void theory_seq::add_length_axiom(expr* n) {
    expr* x;
    VERIFY(m_util.str.is_length(n, x));
    if (!m_util.str.is_unit(x) &&
        !m_util.str.is_empty(x) &&
        !m_util.str.is_string(x) &&
        !m_util.str.is_concat(x)) {
        expr_ref zero(m_autil.mk_int(0), m);
        expr_ref emp(m_util.str.mk_empty(m.get_sort(x)), m);
        literal eq1(mk_eq(zero, n, false));
        literal eq2(mk_eq(x, emp, false));
        add_axiom(mk_literal(m_autil.mk_ge(n, zero)));
        add_axiom(~eq1, eq2);
        add_axiom(~eq2, eq1);
    }
}

//
// the empty sequence is accepted only in the final states.
// membership holds iff the initial state holds.
//
void theory_seq::add_in_re_axiom(expr* n) {
    expr* e1, *e2;
    VERIFY(m_util.str.is_in_re(n, e1, e2));
    eautomaton* a = get_automaton(e2);
    if (!a) return;
    
    expr_ref emp(m_util.str.mk_empty(m.get_sort(e1)), m);
    for (unsigned i = 0; i < a->num_states(); ++i) {
        expr_ref acc = mk_accept(emp, e2, m_autil.mk_int(i));
        expr_ref rej = mk_reject(emp, e2, m_autil.mk_int(i));
        literal alit = mk_literal(acc);
        literal rlit = mk_literal(rej);
        add_axiom(a->is_final_state(i)?alit:~alit);        
        add_axiom(a->is_final_state(i)?~rlit:rlit);
    }
}


void theory_seq::propagate_in_re(expr* n, bool is_true) {
    expr* e1, *e2;
    VERIFY(m_util.str.is_in_re(n, e1, e2));
    eautomaton* a = get_automaton(e2);
    if (!a) return;
    if (m_util.str.is_empty(e1)) return;
    context& ctx = get_context();
    unsigned_vector states;
    a->get_epsilon_closure(a->init(), states);
    literal_vector lits;
    literal lit = ctx.get_literal(n);
    if (is_true) {
        lits.push_back(~lit);
    }
    for (unsigned i = 0; i < states.size(); ++i) {
        if (is_true) {
            expr_ref acc = mk_accept(e1, e2, m_autil.mk_int(a->init()));
            lits.push_back(mk_literal(acc));
        }
        else {
            expr_ref rej = mk_reject(e1, e2, m_autil.mk_int(a->init()));
            literal rlit = mk_literal(rej);
            literal nlit = ~lit;
            propagate_lit(0, 1, &nlit, rlit);
        }
    }
    if (is_true) {
        if (lits.size() == 2) {
            propagate_lit(0, 1, &lit, lits[1]);
        }
        else {
            ctx.mk_th_axiom(get_id(), lits.size(), lits.c_ptr());
        }
    }
}


expr_ref theory_seq::mk_sub(expr* a, expr* b) {
    expr_ref result(m_autil.mk_sub(a, b), m);
    m_rewrite(result);
    return result;
}

enode* theory_seq::ensure_enode(expr* e) {
    context& ctx = get_context();
    if (!ctx.e_internalized(e)) {
        ctx.internalize(e, false);
        ctx.mark_as_relevant(ctx.get_enode(e));
    }
    return ctx.get_enode(e);
}

/*
  TBD: check semantics of extract.

  let e = extract(s, i, l)

  0 <= i < len(s) -> prefix(xe,s) & len(x) = i
  0 <= i < len(s) & l >= len(s) - i -> len(e) = len(s) - i
  0 <= i < len(s) & 0 <= l < len(s) - i -> len(e) = l
  0 <= i < len(s) & l < 0 -> len(e) = 0
  *  i < 0 -> e = s
  *  i >= len(s) -> e = empty
*/

void theory_seq::add_extract_axiom(expr* e) {
    expr* s, *i, *l;
    VERIFY(m_util.str.is_extract(e, s, i, l));
    expr_ref x(mk_skolem(symbol("seq.extract.prefix"), s, e), m);
    expr_ref ls(m_util.str.mk_length(s), m);
    expr_ref lx(m_util.str.mk_length(x), m);
    expr_ref le(m_util.str.mk_length(e), m);
    expr_ref ls_minus_i(mk_sub(ls, i), m);
    expr_ref xe(m_util.str.mk_concat(x, e), m);
    expr_ref zero(m_autil.mk_int(0), m);
    
    literal i_ge_0  = mk_literal(m_autil.mk_ge(i, zero));
    literal i_ge_ls = mk_literal(m_autil.mk_ge(mk_sub(i, ls), zero));
    literal l_ge_ls = mk_literal(m_autil.mk_ge(mk_sub(l, ls), zero));
    literal l_ge_zero = mk_literal(m_autil.mk_ge(l, zero));

    add_axiom(~i_ge_0, i_ge_ls, mk_literal(m_util.str.mk_prefix(xe, s)));
    add_axiom(~i_ge_0, i_ge_ls, mk_eq(lx, i, false));
    add_axiom(~i_ge_0, i_ge_ls, ~l_ge_ls, mk_eq(le, ls_minus_i, false));
    add_axiom(~i_ge_0, i_ge_ls, l_ge_zero, mk_eq(le, zero, false));    
}

/*
   let e = at(s, i)
   
   0 <= i < len(s) -> s = xey & len(x) = i & len(e) = 1
   
*/
void theory_seq::add_at_axiom(expr* e) {
    expr* s, *i;
    VERIFY(m_util.str.is_at(e, s, i));
    expr_ref x(m), y(m), lx(m), le(m), xey(m), zero(m), one(m), len_e(m), len_x(m);
    x     = mk_skolem(symbol("seq.at.left"), s);
    y     = mk_skolem(symbol("seq.at.right"), s);
    xey   = m_util.str.mk_concat(x, e, y);
    zero  = m_autil.mk_int(0);
    one   = m_autil.mk_int(1);
    len_e = m_util.str.mk_length(e);
    len_x = m_util.str.mk_length(x);

    literal i_ge_0 = mk_literal(m_autil.mk_ge(i, zero));
    literal i_ge_len_s = mk_literal(m_autil.mk_ge(mk_sub(i, m_util.str.mk_length(s)), zero));

    add_axiom(~i_ge_0, i_ge_len_s, mk_eq(s, xey, false));
    add_axiom(~i_ge_0, i_ge_len_s, mk_eq(one, len_e, false));
    add_axiom(~i_ge_0, i_ge_len_s, mk_eq(i, len_x, false));
}

/**
   step(s, tail, re, i, j, t) -> s = t ++ tail
*/
void theory_seq::propagate_step(bool_var v, expr* step) {
    expr* re, *t, *s, *tail, *i, *j;
    VERIFY(is_step(step, s, tail, re, i, j, t));  
    expr_ref conc(m_util.str.mk_concat(m_util.str.mk_unit(t), tail), m);
    propagate_eq(v, s, conc);
}


literal theory_seq::mk_literal(expr* _e) {
    expr_ref e(_e, m);
    context& ctx = get_context();
    ensure_enode(e);
    return ctx.get_literal(e);
}

void theory_seq::add_axiom(literal l1, literal l2, literal l3, literal l4) {
    context& ctx = get_context();
    literal_vector lits;
    if (l1 != null_literal) { ctx.mark_as_relevant(l1); lits.push_back(l1); }
    if (l2 != null_literal) { ctx.mark_as_relevant(l2); lits.push_back(l2); }
    if (l3 != null_literal) { ctx.mark_as_relevant(l3); lits.push_back(l3); }
    if (l4 != null_literal) { ctx.mark_as_relevant(l4); lits.push_back(l4); }
    TRACE("seq", ctx.display_literals_verbose(tout, lits.size(), lits.c_ptr()); tout << "\n";);
    ctx.mk_th_axiom(get_id(), lits.size(), lits.c_ptr());
}


expr_ref theory_seq::mk_skolem(symbol const& name, expr* e1, 
                               expr* e2, expr* e3, sort* range) {
    expr* es[3] = { e1, e2, e3 };
    unsigned len = e3?3:(e2?2:1);
    if (!range) {
        range = m.get_sort(e1);
    }
    return expr_ref(m_util.mk_skolem(name, len, es, range), m);
}

bool theory_seq::is_skolem(symbol const& s, expr* e) const {
    return m_util.is_skolem(e) && to_app(e)->get_decl()->get_parameter(0).get_symbol() == s;
}


void theory_seq::propagate_eq(bool_var v, expr* e1, expr* e2) {
    context& ctx = get_context();

    SASSERT(ctx.e_internalized(e2));
    enode* n1 = ensure_enode(e1);
    enode* n2 = ensure_enode(e2);
    if (n1->get_root() == n2->get_root()) {
        return;
    }
    TRACE("seq", 
          tout << mk_pp(ctx.bool_var2enode(v)->get_owner(), m) << " => " 
          << mk_pp(e1, m) << " = " << mk_pp(e2, m) << "\n";); 
    literal lit(v);
    justification* js = 
        ctx.mk_justification(
            ext_theory_eq_propagation_justification(
                get_id(), ctx.get_region(), 1, &lit, 0, 0, n1, n2));

    ctx.assign_eq(n1, n2, eq_justification(js));
}


void theory_seq::assign_eh(bool_var v, bool is_true) {
    context & ctx = get_context();
    expr* e = ctx.bool_var2expr(v);
    expr* e1, *e2;
    expr_ref f(m);

    if (is_true && m_util.str.is_prefix(e, e1, e2)) {
        f = mk_skolem(m_prefix_sym, e1, e2);
        f = m_util.str.mk_concat(e1, f);
        propagate_eq(v, f, e2);
    }
    else if (is_true && m_util.str.is_suffix(e, e1, e2)) {
        f = mk_skolem(m_suffix_sym, e1, e2);
        f = m_util.str.mk_concat(f, e1);
        propagate_eq(v, f, e2);
    }
    else if (is_true && m_util.str.is_contains(e, e1, e2)) {
        expr_ref f1 = mk_skolem(m_contains_left_sym, e1, e2);
        expr_ref f2 = mk_skolem(m_contains_right_sym, e1, e2);
        f = m_util.str.mk_concat(m_util.str.mk_concat(f1, e2), f2);
        propagate_eq(v, f, e1);
    }
    else if (is_accept(e)) {
        m_trail_stack.push(push_back_vector<theory_seq, ptr_vector<expr> >(m_accepts));
        m_accepts.push_back(e);
    }
    else if (is_reject(e)) {
        m_trail_stack.push(push_back_vector<theory_seq, ptr_vector<expr> >(m_rejects));
        m_rejects.push_back(e);
    }
    else if (is_step(e)) {
        if (is_true) {
            propagate_step(v, e);
            m_trail_stack.push(push_back_vector<theory_seq, ptr_vector<expr> >(m_steps));
            m_steps.push_back(e);
        }
    }
    else if (m_util.str.is_in_re(e)) {
        propagate_in_re(e, is_true);
    }
    else {
        SASSERT(!is_true);
        //if (m_util.str.is_prefix(e, e1, e2)) {
            // could add negative prefix axioms:
            // len(e1) <= len(e2) => e2 = seq.prefix.left(e2)*seq.prefix.right(e2)
            //                    &  len(seq.prefix.left(e2)) = len(e1)
            //                    &  seq.prefix.left(e2) != e1
            // or could solve prefix/suffix disunification constraints.
        //}
        m_trail_stack.push(push_back_vector<theory_seq, expr_ref_vector>(m_ineqs));
        m_ineqs.push_back(e);
    }
}

void theory_seq::new_eq_eh(theory_var v1, theory_var v2) { 
    enode* n1 = get_enode(v1);
    enode* n2 = get_enode(v2);
    if (n1 != n2) {
        expr_ref o1(n1->get_owner(), m);
        expr_ref o2(n2->get_owner(), m);
        TRACE("seq", tout << o1 << " = " << o2 << "\n";);
        m_eqs.push_back(eq(o1, o2, m_dm.mk_leaf(enode_pair(n1, n2))));
    }
}

void theory_seq::new_diseq_eh(theory_var v1, theory_var v2) {
    enode* n1 = get_enode(v1);
    enode* n2 = get_enode(v2);
    expr_ref e1(n1->get_owner(), m);
    expr_ref e2(n2->get_owner(), m);
    m_nqs.push_back(ne(e1, e2));
    m_exclude.update(e1, e2);
}

void theory_seq::push_scope_eh() {
    theory::push_scope_eh();
    m_rep.push_scope();
    m_exclude.push_scope();
    m_dm.push_scope();
    m_trail_stack.push_scope();
    m_trail_stack.push(value_trail<theory_seq, unsigned>(m_axioms_head));
    m_eqs.push_scope();
    m_nqs.push_scope();
}

void theory_seq::pop_scope_eh(unsigned num_scopes) {
    m_trail_stack.pop_scope(num_scopes);
    theory::pop_scope_eh(num_scopes);   
    m_dm.pop_scope(num_scopes); 
    m_rep.pop_scope(num_scopes);
    m_exclude.pop_scope(num_scopes);
    m_eqs.pop_scope(num_scopes);
    m_nqs.pop_scope(num_scopes);
}

void theory_seq::restart_eh() {
}

void theory_seq::relevant_eh(app* n) {    
    if (m_util.str.is_length(n)  ||    
        m_util.str.is_index(n)   ||
        m_util.str.is_replace(n) ||
        m_util.str.is_extract(n) ||
        m_util.str.is_at(n) ||
        m_util.str.is_concat(n) ||
        m_util.str.is_empty(n) ||
        m_util.str.is_unit(n) ||
        m_util.str.is_string(n) ||
        m_util.str.is_in_re(n) ||
        is_step(n)) {
        enque_axiom(n);
    }
#if 0
    if (m_util.str.is_in_re(n) ||
        m_util.str.is_contains(n) ||
        m_util.str.is_suffix(n) ||
        m_util.str.is_prefix(n)) {
        context& ctx = get_context();
        TRACE("seq", tout << mk_pp(n, m) << "\n";);
        bool_var bv = ctx.get_bool_var(n);
        switch (ctx.get_assignment(bv)) {
        case l_false:
            assign_eh(bv, false);
            break;
        case l_true:
            assign_eh(bv, true);            
            break;
        case l_undef:
            break;
        }
    }
#endif
}


eautomaton* theory_seq::get_automaton(expr* re) {
    eautomaton* result = 0;
    if (m_re2aut.find(re, result)) {
        return result;
    }
    result = re2automaton(m)(re);
    if (result) {
        display_expr disp(m);
        TRACE("seq", result->display(tout, disp););
    }
    if (result) {
        m_automata.push_back(result);
        m_trail_stack.push(push_back_vector<theory_seq, scoped_ptr_vector<eautomaton> >(m_automata));
    }
    m_re2aut.insert(re, result);
    m_trail_stack.push(insert_obj_map<theory_seq, expr, eautomaton*>(m_re2aut, re));
    return result;
}

expr_ref theory_seq::mk_accept(expr* s, expr* re, expr* state) {
    return expr_ref(mk_skolem(m_accept_sym, s, re, state, m.mk_bool_sort()), m);
}
expr_ref theory_seq::mk_reject(expr* s, expr* re, expr* state) {
    return expr_ref(mk_skolem(m_reject_sym, s, re, state, m.mk_bool_sort()), m);
}

bool theory_seq::is_acc_rej(symbol const& ar, expr* e, expr*& s, expr*& re, unsigned& i, eautomaton*& aut) {
    if (is_skolem(ar, e)) {
        rational r;
        s  = to_app(e)->get_arg(0);
        re = to_app(e)->get_arg(1);
        VERIFY(m_autil.is_numeral(to_app(e)->get_arg(2), r));
        SASSERT(r.is_unsigned());
        i = r.get_unsigned();
        aut = m_re2aut[re];
        return true;
    }
    else {
        return false;
    }
}

bool theory_seq::is_step(expr* e) const {
    return is_skolem(symbol("aut.step"), e);
}

bool theory_seq::is_step(expr* e, expr*& s, expr*& tail, expr*& re, expr*& i, expr*& j, expr*& t) const {
    if (is_step(e)) {
        s    = to_app(e)->get_arg(0);
        tail = to_app(e)->get_arg(1);
        re   = to_app(e)->get_arg(2);
        i    = to_app(e)->get_arg(3);
        j    = to_app(e)->get_arg(4);
        t    = to_app(e)->get_arg(5);
        return true;
    }
    else {
        return false;
    }
}

expr_ref theory_seq::mk_step(expr* s, expr* tail, expr* re, unsigned i, unsigned j, expr* t) {
    expr_ref_vector args(m);
    args.push_back(s);
    args.push_back(tail);
    args.push_back(re);    
    args.push_back(m_autil.mk_int(i));
    args.push_back(m_autil.mk_int(j));
    args.push_back(t);
    return expr_ref(m_util.mk_skolem(symbol("aut.step"), args.size(), args.c_ptr(), m.mk_bool_sort()), m);
}


/**
   acc & s != emp -> \/ step_i_t_j
*/
void theory_seq::add_accept2step(expr* acc) {
    context& ctx = get_context();
    expr* s, *re;
    unsigned src;
    eautomaton* aut = 0;
    VERIFY(is_accept(acc, s, re, src, aut));
    if (!aut) return;
    if (m_util.str.is_empty(s)) return;
    eautomaton::moves mvs;
    aut->get_moves_to(src, mvs);
    expr_ref head(m), tail(m), emp(m), step(m);
    mk_decompose(s, emp, head, tail);
    literal_vector lits;
    literal acc_lit = mk_literal(acc);
    lits.push_back(~acc_lit);
    lits.push_back(mk_eq(emp, s, false));
    for (unsigned i = 0; i < mvs.size(); ++i) {
        eautomaton::move mv = mvs[i];
        step = mk_step(s, tail, re, src, mv.dst(), mv.t());
        lits.push_back(mk_literal(step));
    }
    TRACE("seq", ctx.display_literals_verbose(tout, lits.size(), lits.c_ptr()); tout << "\n";);
    ctx.mk_th_axiom(get_id(), lits.size(), lits.c_ptr());
}


/**
   acc(s, re, i) & step(head, tail, re, i, j, t) => acc(tail, re, j)
*/

void theory_seq::add_step2accept(expr* step) {
    expr* re, *t, *s, *tail, *i, *j;
    VERIFY(is_step(step, s, tail, re, i, j, t));
    expr_ref acc1 = mk_accept(s, re, i);
    expr_ref acc2 = mk_accept(tail, re, j);    
    add_axiom(~mk_literal(acc1), ~mk_literal(step), mk_literal(acc2));    
}


/*
   rej(s, re, i) & s = t ++ tail => rej(tail, re, j)
*/ 
void theory_seq::add_reject2reject(expr* rej) {
    expr* s, *re;
    unsigned src;
    eautomaton* aut = 0;
    VERIFY(is_reject(rej, s, re, src, aut));
    if (!aut) return;
    if (m_util.str.is_empty(s)) return;
    eautomaton::moves mvs;
    aut->get_moves_to(src, mvs);
    expr_ref head(m), tail(m), emp(m), rej2(m), conc(m);
    mk_decompose(s, emp, head, tail);
    literal rej1 = mk_literal(rej);
    for (unsigned i = 0; i < mvs.size(); ++i) {
        eautomaton::move const& mv = mvs[i];        
        conc = m_util.str.mk_concat(m_util.str.mk_unit(mv.t()), tail);
        rej2 = mk_reject(tail, re, m_autil.mk_int(mv.dst()));
        add_axiom(~rej1, ~mk_eq(s, conc, false), mk_literal(rej2));
    }
}

bool theory_seq::propagate_automata() {
    context& ctx = get_context();
    bool change = 
        (m_accepts_qhead < m_accepts.size()) || 
        (m_rejects_qhead < m_rejects.size()) ||
        (m_steps_qhead < m_steps.size());

    if (change) {
        m_trail_stack.push(value_trail<theory_seq, unsigned>(m_accepts_qhead));
        m_trail_stack.push(value_trail<theory_seq, unsigned>(m_rejects_qhead));
        m_trail_stack.push(value_trail<theory_seq, unsigned>(m_steps_qhead));        
    }
    while (m_accepts_qhead < m_accepts.size() && !ctx.inconsistent()) {
        expr* acc = m_accepts[m_accepts_qhead];
        lbool r = ctx.get_assignment(acc);
        SASSERT(l_undef != r);
        if (r == l_true) {
            add_accept2step(acc);
        }
        ++m_accepts_qhead;
    }
    while (m_rejects_qhead < m_rejects.size() && !ctx.inconsistent()) {
        expr* rej = m_rejects[m_rejects_qhead];
        lbool r = ctx.get_assignment(rej);
        SASSERT(l_undef != r);
        if (r == l_true) {
            add_reject2reject(rej);
        }
        ++m_rejects_qhead;
    }
    while (m_steps_qhead < m_steps.size() && !ctx.inconsistent()) {
        expr* step = m_steps[m_steps_qhead];
        lbool r = ctx.get_assignment(step);
        switch (r) {
        case l_true: {
            expr* re, *t, *s, *tail, *i, *j;
            VERIFY(is_step(step, s, tail, re, i, j, t));
            expr_ref acc1 = mk_accept(s, re, i);
            if (ctx.get_assignment(acc1) != l_false) {
                add_step2accept(step);
            }
            break;
        }
        case l_false:
            break;
        default:
            UNREACHABLE();
        }        
        ++m_steps_qhead;
    }
    return change || ctx.inconsistent();
}
