#include "jitana/analysis/points_to.hpp"
#include "jitana/analysis/data_flow.hpp"
#include "jitana/algorithm/unique_sort.hpp"

#include <vector>
#include <queue>

#include <boost/pending/disjoint_sets.hpp>

using namespace jitana;

namespace {
    dex_insn_hdl no_insn_hdl = {{{0, 0}, 0}, 0};
}

namespace {
    using pag_parent_map
            = boost::property_map<pointer_assignment_graph,
                                  pag_vertex_descriptor
                                          pag_vertex_property::*>::type;
    using pag_rank_map = boost::property_map<pointer_assignment_graph,
                                             int pag_vertex_property::*>::type;
    using pag_disjoint_sets
            = boost::disjoint_sets<pag_rank_map, pag_parent_map>;
}

namespace {
    pag_vertex_descriptor make_vertex_for_reg(const dex_reg_hdl& hdl,
                                              const dex_insn_hdl& context,
                                              pointer_assignment_graph& g)
    {
        pag_reg reg{hdl};
        if (auto v = lookup_pag_reg_vertex(reg, context, g)) {
            return *v;
        }

        auto v = add_vertex(g);
        g[v].vertex = reg;
        g[v].context = context;
        g[v].parent = v;
        g[boost::graph_bundle].reg_vertex_lut.insert({reg, v});
        return v;
    }

    pag_vertex_descriptor make_vertex_for_alloc(const dex_insn_hdl& hdl,
                                                pointer_assignment_graph& g)
    {
        pag_alloc alloc{hdl};
        if (auto v = lookup_pag_alloc_vertex(alloc, no_insn_hdl, g)) {
            return *v;
        }

        auto v = add_vertex(g);
        g[v].vertex = alloc;
        g[v].context = no_insn_hdl;
        g[v].parent = v;
        g[v].points_to_set.push_back(v);
        g[boost::graph_bundle].alloc_vertex_lut.insert({alloc, v});
        return v;
    }

    pag_vertex_descriptor make_vertex_for_reg_dot_field(
            const dex_reg_hdl& reg_hdl, const dex_field_hdl& field_hdl,
            const dex_insn_hdl& context, pointer_assignment_graph& g)
    {
        pag_reg_dot_field rdf{reg_hdl, field_hdl};
        if (auto v = lookup_pag_reg_dot_field_vertex(rdf, context, g)) {
            return *v;
        }

        auto v = add_vertex(g);
        g[v].vertex = rdf;
        g[v].context = context;
        g[v].parent = v;
        g[boost::graph_bundle].reg_dot_field_vertex_lut.insert({rdf, v});
        return v;
    }

    pag_vertex_descriptor
    make_vertex_for_alloc_dot_field(const dex_insn_hdl& insn_hdl,
                                    const dex_field_hdl& field_hdl,
                                    pointer_assignment_graph& g)
    {
        pag_alloc_dot_field adf{insn_hdl, field_hdl};
        if (auto v = lookup_pag_alloc_dot_field_vertex(adf, no_insn_hdl, g)) {
            return *v;
        }

        auto v = add_vertex(g);
        g[v].vertex = adf;
        g[v].context = no_insn_hdl;
        g[v].parent = v;
        g[boost::graph_bundle].alloc_dot_field_vertex_lut.insert({adf, v});
        return v;
    }

    pag_vertex_descriptor
    make_vertex_for_static_field(const dex_field_hdl& hdl,
                                 pointer_assignment_graph& g)
    {
        pag_static_field sf{hdl};
        if (auto v = lookup_pag_static_field_vertex(sf, no_insn_hdl, g)) {
            return *v;
        }

        auto v = add_vertex(g);
        g[v].vertex = sf;
        g[v].context = no_insn_hdl;
        g[v].parent = v;
        g[boost::graph_bundle].static_field_vertex_lut.insert({sf, v});
        return v;
    }

    pag_vertex_descriptor
    make_vertex_for_reg_dot_array(const dex_reg_hdl& hdl,
                                  const dex_insn_hdl& context,
                                  pointer_assignment_graph& g)
    {
        pag_reg_dot_array rda{hdl};
        if (auto v = lookup_pag_reg_dot_array_vertex(rda, context, g)) {
            return *v;
        }

        auto v = add_vertex(g);
        g[v].vertex = rda;
        g[v].context = context;
        g[v].parent = v;
        g[boost::graph_bundle].reg_dot_array_vertex_lut.insert({rda, v});
        return v;
    }

    pag_vertex_descriptor
    make_vertex_for_alloc_dot_array(const dex_insn_hdl& insn_hdl,
                                    pointer_assignment_graph& g)
    {
        pag_alloc_dot_array ada{insn_hdl};
        if (auto v = lookup_pag_alloc_dot_array_vertex(ada, no_insn_hdl, g)) {
            return *v;
        }

        auto v = add_vertex(g);
        g[v].vertex = ada;
        g[v].context = no_insn_hdl;
        g[v].parent = v;
        g[boost::graph_bundle].alloc_dot_array_vertex_lut.insert({ada, v});
        return v;
    }
}

namespace {
    struct invocation {
        dex_insn_hdl callsite;
        method_vertex_descriptor mv;

        friend bool operator==(const invocation& x, const invocation& y)
        {
            return x.mv == y.mv && x.callsite == y.callsite;
        }
    };
}

namespace std {
    template <>
    struct hash<invocation> {
        size_t operator()(const invocation& x) const
        {
            return std::hash<jitana::dex_insn_hdl>()(x.callsite)
                    * std::hash<jitana::method_vertex_descriptor>()(x.mv);
        }
    };
}

namespace {
    struct points_to_algorithm_data {
        points_to_algorithm_data(pointer_assignment_graph& pag,
                                 contextual_call_graph& cg, virtual_machine& vm,
                                 bool on_the_fly_cg)
                : pag(pag),
                  cg(cg),
                  vm(vm),
                  on_the_fly_cg(on_the_fly_cg),
                  ds(get(&pag_vertex_property::rank, pag),
                     get(&pag_vertex_property::parent, pag))
        {
        }

        void move_current_insn(const insn_graph* ig, insn_vertex_descriptor iv)
        {
            points_to_algorithm_data::ig = ig;
            points_to_algorithm_data::iv = iv;
            points_to_algorithm_data::insn_hdl = {
                    (*ig)[boost::graph_bundle].hdl, static_cast<uint16_t>(iv)};
        }

        void add_to_worklist(pag_vertex_descriptor v)
        {
            if (!pag[v].dirty) {
                worklist.push(v);
                pag[v].dirty = true;
            }
        }

        pointer_assignment_graph& pag;
        contextual_call_graph& cg;
        virtual_machine& vm;
        bool on_the_fly_cg;
        pag_disjoint_sets ds;

        const insn_graph* ig = nullptr;
        insn_vertex_descriptor iv;
        dex_insn_hdl insn_hdl;

        dex_insn_hdl context = no_insn_hdl;

        std::queue<pag_vertex_descriptor> worklist;
        std::unordered_set<invocation> visited;
    };
}

namespace {
    class pag_insn_visitor {
    public:
        pag_insn_visitor(points_to_algorithm_data& d,
                         std::queue<invocation>& invoc_queue)
                : d_(d), invoc_queue_(invoc_queue)
        {
        }

        void operator()(const insn_move& x)
        {
            switch (x.op) {
            case opcode::op_move_object:
            case opcode::op_move_object_from16:
            case opcode::op_move_object_16:
            case opcode::op_move_result_object:
                add_assign_edge(x.regs[0], x.regs[1]);
                break;
            default:
                break;
            }
        }

        void operator()(const insn_return& x)
        {
            switch (x.op) {
            case opcode::op_return_object:
                add_assign_edge(register_idx(register_idx::idx_result),
                                x.regs[0]);
                break;
            default:
                break;
            }
        }

        void operator()(const insn_check_cast& x)
        {
            add_assign_edge(x.regs[0], x.regs[0]);
        }

        void operator()(const insn_const_string& x)
        {
            add_alloc_edge(x.regs[0]);
        }

        void operator()(const insn_const_class& x)
        {
            add_alloc_edge(x.regs[0]);
        }

        void operator()(const insn_new_instance& x)
        {
            // Run <clinit> of the target class.
            {
                auto cv = d_.vm.find_class(x.const_val, false);
                if (!cv) {
                    std::cerr
                            << "class not found: " << d_.vm.jvm_hdl(x.const_val)
                            << "\n";
                    return;
                }

                const auto& cg = d_.vm.classes();
                auto clinit_mv = d_.vm.find_method(
                        jvm_method_hdl(cg[*cv].jvm_hdl, "<clinit>()V"), false);
                if (clinit_mv) {
                    invoc_queue_.push({no_insn_hdl, *clinit_mv});
                }
            }

            add_alloc_edge(x.regs[0]);
        }

        void operator()(const insn_new_array& x)
        {
            add_alloc_edge(x.regs[0]);
        }

        void operator()(const insn_filled_new_array&)
        {
        }

        void operator()(const insn_aget& x)
        {
            add_aload_edge(x.regs[0], x.regs[1], x.regs[2]);
        }

        void operator()(const insn_aput& x)
        {
            add_astore_edge(x.regs[0], x.regs[1], x.regs[2]);
        }

        void operator()(const insn_iget& x)
        {
            auto fv = d_.vm.find_field(x.const_val, false);
            if (!fv) {
                std::cerr << "field not found: " << d_.vm.jvm_hdl(x.const_val)
                          << "\n";
                return;
            }

            add_iload_edge(x.regs[0], x.regs[1], d_.vm.fields()[*fv].hdl);
        }

        void operator()(const insn_iput& x)
        {
            auto fv = d_.vm.find_field(x.const_val, false);
            if (!fv) {
                std::cerr << "field not found: " << d_.vm.jvm_hdl(x.const_val)
                          << "\n";
                return;
            }

            add_istore_edge(x.regs[0], x.regs[1], d_.vm.fields()[*fv].hdl);
        }

        void operator()(const insn_sget& x)
        {
            auto fv = d_.vm.find_field(x.const_val, false);
            if (!fv) {
                std::cerr << "field not found: " << d_.vm.jvm_hdl(x.const_val)
                          << "\n";
                return;
            }

            // Run <clinit> of the target class.
            {
                const auto& fg = d_.vm.fields();
                auto clinit_mv = d_.vm.find_method(
                        jvm_method_hdl(fg[*fv].jvm_hdl.type_hdl, "<clinit>()V"),
                        false);
                if (clinit_mv) {
                    invoc_queue_.push({no_insn_hdl, *clinit_mv});
                }
            }

            add_sload_edge(x.regs[0], d_.vm.fields()[*fv].hdl);
        }

        void operator()(const insn_sput& x)
        {
            auto fv = d_.vm.find_field(x.const_val, false);
            if (!fv) {
                std::cerr << "field not found: " << d_.vm.jvm_hdl(x.const_val)
                          << "\n";
                return;
            }

            // Run <clinit> of the target class.
            {
                const auto& fg = d_.vm.fields();
                auto clinit_mv = d_.vm.find_method(
                        jvm_method_hdl(fg[*fv].jvm_hdl.type_hdl, "<clinit>()V"),
                        false);
                if (clinit_mv) {
                    invoc_queue_.push({no_insn_hdl, *clinit_mv});
                }
            }

            add_sstore_edge(x.regs[0], d_.vm.fields()[*fv].hdl);
        }

        void operator()(const insn_invoke& x)
        {
            auto mv = d_.vm.find_method(x.const_val, false);
            if (!mv) {
                std::cerr << "method not found: " << d_.vm.jvm_hdl(x.const_val)
                          << "\n";
                return;
            }

            const auto& mg = d_.vm.methods();
            switch (x.op) {
            case opcode::op_invoke_static:
            case opcode::op_invoke_static_range:
                // Run <clinit> of the target class.
                {
                    auto clinit_mv = d_.vm.find_method(
                            jvm_method_hdl(mg[*mv].jvm_hdl.type_hdl,
                                           "<clinit>()V"),
                            false);
                    if (clinit_mv) {
                        invoc_queue_.push({no_insn_hdl, *clinit_mv});
                    }
                }
                break;
            default:
                break;
            }

            auto inheritance_mg
                    = make_edge_filtered_graph<method_super_edge_property>(mg);

            boost::vector_property_map<int> color_map(
                    static_cast<unsigned>(num_vertices(inheritance_mg)));
            auto f = [&](method_vertex_descriptor v,
                         const decltype(inheritance_mg)&) {
                invoc_queue_.push({d_.insn_hdl, v});
                add_invoke_edges(v, x);
                return false;
            };
            boost::depth_first_visit(inheritance_mg, *mv,
                                     boost::default_dfs_visitor{}, color_map,
                                     f);
        }

        void operator()(const insn_invoke_quick&)
        {
        }

        template <typename T>
        void operator()(const T&)
        {
        }

    private:
        template <typename T>
        void add_invoke_edges(method_vertex_descriptor mv, const T& insn)
        {
            auto add_call_edge = [&](const dex_reg_hdl& dst_reg_hdl,
                                     const dex_reg_hdl& src_reg_hdl) {
                auto src_v
                        = make_vertex_for_reg(src_reg_hdl, d_.context, d_.pag);
                auto dst_v
                        = make_vertex_for_reg(dst_reg_hdl, d_.insn_hdl, d_.pag);

                add_edge(src_v, dst_v, {pag_edge_property::kind_assign},
                         d_.pag);

            };

            const auto& mg = d_.vm.methods();
            const auto& tgt_mvprop = mg[mv];
            const auto& params = tgt_mvprop.params;
            const auto& tgt_igprop = tgt_mvprop.insns[boost::graph_bundle];

            if (tgt_mvprop.access_flags & acc_abstract) {
                return;
            }

            // Parameters.
            {
                std::vector<int> reg_offsets;
                {
                    int offset = 0;

                    // Non-static method has a this pointer as the first
                    // argument.
                    if (!(tgt_mvprop.access_flags & acc_static)) {
                        reg_offsets.push_back(0);
                        ++offset;
                    }

                    for (const auto& p : params) {
                        char desc = p.descriptor[0];

                        reg_offsets.push_back(
                                (desc == 'L' || desc == '[') ? offset : -1);

                        ++offset;
                        if (desc == 'J' || desc == 'D') {
                            // Ignore next register if the parameter is a
                            // wide type.
                            ++offset;
                        }
                    }
                }

                dex_insn_hdl tgt_entry_insn_hdl(tgt_mvprop.hdl, 0);
                dex_reg_hdl dst_reg_hdl(tgt_entry_insn_hdl, 0);

                auto actual_reg_start
                        = tgt_igprop.registers_size - tgt_igprop.ins_size;
                if (insn.is_regs_range()) {
                    auto formal_reg_start = insn.regs[0].value;
                    for (auto off : reg_offsets) {
                        if (off != -1) {
                            dst_reg_hdl.idx = actual_reg_start + off;
                            for_each_incoming_reg(
                                    formal_reg_start + off,
                                    [&](const dex_reg_hdl& src_reg_hdl) {
                                        add_call_edge(dst_reg_hdl, src_reg_hdl);
                                    });
                        }
                    }
                }
                else {
                    for (size_t i = 0; i < reg_offsets.size(); ++i) {
                        auto off = reg_offsets[i];
                        if (off != -1) {
                            dst_reg_hdl.idx = actual_reg_start + off;
                            for_each_incoming_reg(
                                    insn.regs[i].value,
                                    [&](const dex_reg_hdl& src_reg_hdl) {
                                        add_call_edge(dst_reg_hdl, src_reg_hdl);
                                    });
                        }
                    }
                }
            }

            // Return value.
            auto ret_desc = tgt_mvprop.jvm_hdl.return_descriptor()[0];
            if (ret_desc == 'L' || ret_desc == '[') {
                dex_insn_hdl tgt_exit_insn_hdl(
                        tgt_mvprop.hdl, num_vertices(tgt_mvprop.insns) - 1);
                dex_reg_hdl src_reg_hdl(tgt_exit_insn_hdl,
                                        register_idx::idx_result);
                dex_reg_hdl dst_reg_hdl(d_.insn_hdl, register_idx::idx_result);

                auto src_v
                        = make_vertex_for_reg(src_reg_hdl, d_.insn_hdl, d_.pag);
                auto dst_v
                        = make_vertex_for_reg(dst_reg_hdl, d_.context, d_.pag);

                add_edge(src_v, dst_v, {pag_edge_property::kind_assign},
                         d_.pag);
            }
        }

        template <typename Func>
        void for_each_incoming_reg(register_idx reg, Func f)
        {
            using boost::make_iterator_range;
            using boost::type_erasure::any_cast;

            dex_reg_hdl reg_hdl(d_.insn_hdl, reg.value);

            for (const auto& e : make_iterator_range(in_edges(d_.iv, *d_.ig))) {
                using edge_prop_t = insn_data_flow_edge_property;
                const auto* de = any_cast<const edge_prop_t*>(&(*d_.ig)[e]);
                if (de != nullptr && de->reg == reg) {
                    reg_hdl.insn_hdl.idx = source(e, *d_.ig);
                    f(reg_hdl);
                }
            }
        }

        void add_alloc_edge(register_idx dst_reg)
        {
            dex_reg_hdl dst_reg_hdl(d_.insn_hdl, dst_reg.value);

            auto src_v = make_vertex_for_alloc(d_.insn_hdl, d_.pag);
            auto dst_v = make_vertex_for_reg(dst_reg_hdl, d_.context, d_.pag);

            add_edge(src_v, dst_v, {pag_edge_property::kind_alloc}, d_.pag);

            d_.add_to_worklist(dst_v);
        }

        void add_assign_edge(const dex_reg_hdl& dst_reg_hdl,
                             const dex_reg_hdl& src_reg_hdl)
        {
            auto src_v = make_vertex_for_reg(src_reg_hdl, d_.context, d_.pag);
            auto dst_v = make_vertex_for_reg(dst_reg_hdl, d_.context, d_.pag);

            add_edge(src_v, dst_v, {pag_edge_property::kind_assign}, d_.pag);
        }

        void add_assign_edge(register_idx dst_reg, register_idx src_reg)
        {
            using boost::make_iterator_range;
            using boost::type_erasure::any_cast;

            dex_reg_hdl dst_reg_hdl(d_.insn_hdl, dst_reg.value);

            // If the destination is the result register, we know that it
            // comes from the return instruction. Thus we associate the
            // result resiter to the exit instruction.
            if (dst_reg == register_idx::idx_result) {
                dst_reg_hdl.insn_hdl.idx = num_vertices(*d_.ig) - 1;
            }

            for_each_incoming_reg(src_reg, [&](const dex_reg_hdl& src_reg_hdl) {
                add_assign_edge(dst_reg_hdl, src_reg_hdl);
            });
        }

        void add_astore_edge(register_idx src_reg, register_idx obj_reg,
                             register_idx /*idx_reg*/)
        {
            for_each_incoming_reg(src_reg, [&](const dex_reg_hdl& src_reg_hdl) {
                for_each_incoming_reg(
                        obj_reg, [&](const dex_reg_hdl& obj_reg_hdl) {
                            auto src_v = make_vertex_for_reg(
                                    src_reg_hdl, d_.context, d_.pag);
                            auto dst_v = make_vertex_for_reg_dot_array(
                                    obj_reg_hdl, d_.context, d_.pag);
                            auto obj_v = make_vertex_for_reg(
                                    obj_reg_hdl, d_.context, d_.pag);

                            d_.pag[obj_v].dereferenced_by.push_back(dst_v);
                            unique_sort(d_.pag[obj_v].dereferenced_by);

                            add_edge(src_v, dst_v,
                                     {pag_edge_property::kind_astore}, d_.pag);
                        });
            });
        }

        void add_aload_edge(register_idx dst_reg, register_idx obj_reg,
                            register_idx /*idx_reg*/)
        {
            dex_reg_hdl dst_reg_hdl(d_.insn_hdl, dst_reg.value);

            for_each_incoming_reg(obj_reg, [&](const dex_reg_hdl& obj_reg_hdl) {
                auto src_v = make_vertex_for_reg_dot_array(obj_reg_hdl,
                                                           d_.context, d_.pag);
                auto dst_v
                        = make_vertex_for_reg(dst_reg_hdl, d_.context, d_.pag);
                auto obj_v
                        = make_vertex_for_reg(obj_reg_hdl, d_.context, d_.pag);

                d_.pag[obj_v].dereferenced_by.push_back(src_v);
                unique_sort(d_.pag[obj_v].dereferenced_by);

                add_edge(src_v, dst_v, {pag_edge_property::kind_aload}, d_.pag);
            });
        }

        void add_istore_edge(register_idx src_reg, register_idx obj_reg,
                             const dex_field_hdl& field_hdl)
        {
            const auto& fv = d_.vm.find_field(field_hdl, false);
            if (!fv) {
                std::cerr << "istore: field not found: "
                          << d_.vm.jvm_hdl(field_hdl) << "\n";
                return;
            }

            const auto& fg = d_.vm.fields();
            if (fg[*fv].type_char == 'L' || fg[*fv].type_char == '[') {
                for_each_incoming_reg(src_reg, [&](const dex_reg_hdl&
                                                           src_reg_hdl) {
                    for_each_incoming_reg(
                            obj_reg, [&](const dex_reg_hdl& obj_reg_hdl) {
                                auto src_v = make_vertex_for_reg(
                                        src_reg_hdl, d_.context, d_.pag);
                                auto dst_v = make_vertex_for_reg_dot_field(
                                        obj_reg_hdl, field_hdl, d_.context,
                                        d_.pag);
                                auto obj_v = make_vertex_for_reg(
                                        obj_reg_hdl, d_.context, d_.pag);

                                d_.pag[obj_v].dereferenced_by.push_back(dst_v);
                                unique_sort(d_.pag[obj_v].dereferenced_by);

                                add_edge(src_v, dst_v,
                                         {pag_edge_property::kind_istore},
                                         d_.pag);

                            });
                });
            }
        }

        void add_iload_edge(register_idx dst_reg, register_idx obj_reg,
                            const dex_field_hdl& field_hdl)
        {
            const auto& fv = d_.vm.find_field(field_hdl, false);
            if (!fv) {
                std::cerr << "iload: field not found: "
                          << d_.vm.jvm_hdl(field_hdl) << "\n";
                return;
            }

            const auto& fg = d_.vm.fields();
            if (fg[*fv].type_char == 'L' || fg[*fv].type_char == '[') {
                dex_reg_hdl dst_reg_hdl(d_.insn_hdl, dst_reg.value);

                for_each_incoming_reg(
                        obj_reg, [&](const dex_reg_hdl& obj_reg_hdl) {
                            auto src_v = make_vertex_for_reg_dot_field(
                                    obj_reg_hdl, field_hdl, d_.context, d_.pag);
                            auto dst_v = make_vertex_for_reg(
                                    dst_reg_hdl, d_.context, d_.pag);
                            auto obj_v = make_vertex_for_reg(
                                    obj_reg_hdl, d_.context, d_.pag);

                            d_.pag[obj_v].dereferenced_by.push_back(src_v);
                            unique_sort(d_.pag[obj_v].dereferenced_by);

                            add_edge(src_v, dst_v,
                                     {pag_edge_property::kind_iload}, d_.pag);

                        });
            }
        }

        void add_sstore_edge(register_idx src_reg,
                             const dex_field_hdl& field_hdl)
        {
            const auto& fg = d_.vm.fields();
            const auto& fv = d_.vm.find_field(field_hdl, false);
            if (!fv) {
                std::stringstream ss;
                ss << "ailed to find the static field ";
                ss << d_.vm.jvm_hdl(field_hdl);
                throw std::runtime_error(ss.str());
            }

            if (fg[*fv].type_char == 'L' || fg[*fv].type_char == '[') {
                for_each_incoming_reg(
                        src_reg, [&](const dex_reg_hdl& src_reg_hdl) {
                            auto src_v = make_vertex_for_reg(
                                    src_reg_hdl, d_.context, d_.pag);
                            auto dst_v = make_vertex_for_static_field(field_hdl,
                                                                      d_.pag);

                            add_edge(src_v, dst_v,
                                     {pag_edge_property::kind_sstore}, d_.pag);
                        });
            }
        }

        void add_sload_edge(register_idx dst_reg,
                            const dex_field_hdl& field_hdl)
        {
            const auto& fg = d_.vm.fields();
            const auto& fv = d_.vm.find_field(field_hdl, false);
            if (!fv) {
                std::stringstream ss;
                ss << "ailed to find the static field ";
                ss << d_.vm.jvm_hdl(field_hdl);
                throw std::runtime_error(ss.str());
            }

            if (fg[*fv].type_char == 'L' || fg[*fv].type_char == '[') {
                dex_reg_hdl dst_reg_hdl(d_.insn_hdl, dst_reg.value);

                auto src_v = make_vertex_for_static_field(field_hdl, d_.pag);
                auto dst_v
                        = make_vertex_for_reg(dst_reg_hdl, d_.context, d_.pag);

                add_edge(src_v, dst_v, {pag_edge_property::kind_sload}, d_.pag);
            }
        }

    private:
        points_to_algorithm_data& d_;
        std::queue<invocation>& invoc_queue_;
    };

    class pag_updater {
    public:
        pag_updater(pointer_assignment_graph& pag, contextual_call_graph& cg,
                    virtual_machine& vm, bool on_the_fly_cg)
                : d_(pag, cg, vm, on_the_fly_cg)
        {
        }

        bool update(const method_vertex_descriptor& entry_mv)
        {
            make_vertices_from_method(entry_mv);
            simplify();

            int counter = 0;
            while (!d_.worklist.empty()) {
                constexpr int period = 10000;
                if (counter % period == 0) {
                    const auto& gprop = d_.pag[boost::graph_bundle];
                    std::printf("%8d%8u%12lu%12lu%12lu%12lu\n", counter,
                                static_cast<unsigned>(d_.worklist.size()),
                                num_vertices(d_.pag),
                                gprop.alloc_vertex_lut.size(),
                                gprop.alloc_dot_field_vertex_lut.size(),
                                gprop.alloc_dot_array_vertex_lut.size());
                }
                counter++;

                // Pop a vertex from the d_.worklist.
                auto v = d_.worklist.front();
                d_.worklist.pop();
                d_.pag[v].dirty = false;

                if (!update_points_to_set(v)) {
                    // Points-to set did not change: continue.
                    continue;
                }

                update_dereferencer(v);

                if (d_.on_the_fly_cg && d_.pag[v].virtual_invoke_receiver) {
#if 0
                    if (auto reg = get<pag_reg>(&d_.pag[v].vertex)) {
                        auto mv = d_.vm.find_method(reg->hdl, false);
                        make_vertices_from_method(*mv);
                        simplify();
                    }
#endif
                }

                // Add the targets to the worklist.
                update_worklist(v);
            }

            return true;
        }

    private:
        bool update_points_to_set(const pag_vertex_descriptor& v)
        {
            auto& g = d_.pag;
            auto& p2s = d_.pag[d_.pag[v].parent].points_to_set;
            auto p2s_size = p2s.size();

            // Process the incoming edges  to build the points-to set.
            for (const auto& ie : boost::make_iterator_range(in_edges(v, g))) {
                auto src_v = source(ie, g);
                auto& src_p2s = g[g[src_v].parent].points_to_set;
                p2s.insert(end(p2s), begin(src_p2s), end(src_p2s));
            }
            unique_sort(p2s);

            // If the points-to set size is changed, we have updated it.
            return p2s.size() != p2s_size;
        }

        void update_dereferencer(const pag_vertex_descriptor& v)
        {
            using edge_list = std::vector<std::pair<pag_vertex_descriptor,
                                                    pag_vertex_descriptor>>;

            struct visitor : boost::static_visitor<void> {
                visitor(pag_vertex_descriptor dereferencer_v,
                        pag_vertex_descriptor OBJ_V,
                        points_to_algorithm_data& d, edge_list& edges_to_add)
                        : dereferencer_v_(dereferencer_v),
                          obj_v(OBJ_V),
                          d_(d),
                          edges_to_add_(edges_to_add)
                {
                }

                void operator()(const pag_reg&) const
                {
                }

                void operator()(const pag_alloc&) const
                {
                }

                void operator()(const pag_reg_dot_field& x) const
                {
                    auto& g = d_.pag;
                    auto obj_p2s = d_.pag[d_.pag[obj_v].parent].points_to_set;

                    auto field_hdl = x.field_hdl;
                    for (auto alloc_v : obj_p2s) {
                        const auto& alloc_vertex
                                = get<pag_alloc>(g[alloc_v].vertex);
                        auto adf_v = make_vertex_for_alloc_dot_field(
                                alloc_vertex.hdl, field_hdl, g);

                        auto inv_adj
                                = inv_adjacent_vertices(dereferencer_v_, g);
                        for (auto v : boost::make_iterator_range(inv_adj)) {
                            edges_to_add_.push_back(std::make_pair(v, adf_v));
                        }

                        auto adj = adjacent_vertices(dereferencer_v_, g);
                        for (auto v : boost::make_iterator_range(adj)) {
                            edges_to_add_.push_back(std::make_pair(adf_v, v));
                        }
                    }
                }

                void operator()(const pag_alloc_dot_field&) const
                {
                }

                void operator()(const pag_static_field&) const
                {
                }

                void operator()(const pag_reg_dot_array&) const
                {
                    auto& g = d_.pag;
                    auto obj_p2s = d_.pag[d_.pag[obj_v].parent].points_to_set;

                    for (auto alloc_v : obj_p2s) {
                        const auto& alloc_vertex
                                = get<pag_alloc>(g[alloc_v].vertex);
                        auto adf_v = make_vertex_for_alloc_dot_array(
                                alloc_vertex.hdl, g);

                        auto inv_adj
                                = inv_adjacent_vertices(dereferencer_v_, g);
                        for (auto v : boost::make_iterator_range(inv_adj)) {
                            edges_to_add_.push_back(std::make_pair(v, adf_v));
                        }

                        auto adj = adjacent_vertices(dereferencer_v_, g);
                        for (auto v : boost::make_iterator_range(adj)) {
                            edges_to_add_.push_back(std::make_pair(adf_v, v));
                        }
                    }
                }

                void operator()(const pag_alloc_dot_array&) const
                {
                }

            private:
                pag_vertex_descriptor dereferencer_v_;
                pag_vertex_descriptor obj_v;
                points_to_algorithm_data& d_;
                edge_list& edges_to_add_;
            };

            auto& g = d_.pag;
            edge_list edges_to_add;

            // Process the incoming edges.
            auto dereferenced_by = g[v].dereferenced_by;
            for (auto dereferencer_v : dereferenced_by) {
                visitor vis(dereferencer_v, v, d_, edges_to_add);
                boost::apply_visitor(vis, g[dereferencer_v].vertex);
            }

            for (const auto& e : edges_to_add) {
                auto adj = adjacent_vertices(e.first, d_.pag);
                if (std::find(adj.first, adj.second, e.second) == adj.second) {
                    // The edge hasn't been added: add it now.
                    add_edge(e.first, e.second,
                             {pag_edge_property::kind_assign}, d_.pag);
                    d_.add_to_worklist(e.second);
                }
            }
        }

        void update_worklist(const pag_vertex_descriptor& v)
        {
            struct visitor : boost::static_visitor<void> {
                visitor(pag_vertex_descriptor tgt_v,
                        points_to_algorithm_data& d)
                        : tgt_v_(tgt_v), d_(d)
                {
                }

                void operator()(const pag_reg&) const
                {
                    d_.add_to_worklist(tgt_v_);
                }

                void operator()(const pag_alloc&) const
                {
                }

                void operator()(const pag_reg_dot_field&) const
                {
                }

                void operator()(const pag_alloc_dot_field&) const
                {
                    d_.add_to_worklist(tgt_v_);
                }

                void operator()(const pag_static_field&) const
                {
                    d_.add_to_worklist(tgt_v_);
                }

                void operator()(const pag_reg_dot_array&) const
                {
                }

                void operator()(const pag_alloc_dot_array&) const
                {
                    d_.add_to_worklist(tgt_v_);
                }

            private:
                pag_vertex_descriptor tgt_v_;
                points_to_algorithm_data& d_;
            };

            auto& g = d_.pag;

            // Process the outgoing edges.
            for (const auto& oe : boost::make_iterator_range(out_edges(v, g))) {
                auto tgt_v = target(oe, g);
                visitor vis(tgt_v, d_);
                boost::apply_visitor(vis, g[tgt_v].vertex);
            }
        }

        void make_vertices_from_method(const method_vertex_descriptor& root_mv)
        {
            std::queue<invocation> invoc_queue;
            invoc_queue.push({no_insn_hdl, root_mv});

            for (; !invoc_queue.empty(); invoc_queue.pop()) {
                auto invoc = invoc_queue.front();

                // If the method is already visited, we just ignore.
                if (d_.visited.find(invoc) != end(d_.visited)) {
                    continue;
                }
                d_.visited.insert(invoc);

                d_.context = invoc.callsite;
                auto mv = invoc.mv;

                const auto& mg = d_.vm.methods();
                const auto& mvprop = mg[mv];
                const auto& ig = mvprop.insns;

                pag_insn_visitor vis(d_, invoc_queue);
                for (auto iv : boost::make_iterator_range(vertices(ig))) {
                    d_.move_current_insn(&ig, iv);
                    boost::apply_visitor(vis, ig[iv].insn);
                }

                if (d_.context != no_insn_hdl) {
                    // callsite->
                }
            }
        }

        void simplify()
        {
            // Colapse SCC.
        }

    private:
        points_to_algorithm_data d_;
    };
}

bool jitana::update_points_to_graphs(pointer_assignment_graph& pag,
                                     contextual_call_graph& cg,
                                     virtual_machine& vm,
                                     const method_vertex_descriptor& mv)
{
    pag_updater updater(pag, cg, vm, true);
    return updater.update(mv);
}