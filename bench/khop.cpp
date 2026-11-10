// khop: the module without the card. Breadth-first walk over transfer edges, as a module author
// would write it with the official C++ API of Memgraph (mgp.hpp).
//
//   CALL khop.out(start_node, hops, t0, t1) YIELD reached, edges, micros
//
// From start_node it follows outgoing :transfer edges with t0 < createTime < t1, up to `hops` steps,
// every account at most once. reached: accounts found; edges: edges looked at; micros: time inside.
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <mgp.hpp>

namespace {

void Out(mgp_list* args, mgp_graph* memgraph_graph, mgp_result* result, mgp_memory* memory) {
    try {
        mgp::MemoryDispatcherGuard guard(memory);
        const auto arguments = mgp::List(args);
        mgp::Node start = arguments[0].ValueNode();
        const int64_t hops = arguments[1].ValueInt();
        const int64_t t0 = arguments[2].ValueInt();
        const int64_t t1 = arguments[3].ValueInt();

        auto begin = std::chrono::steady_clock::now();
        std::unordered_set<int64_t> seen{start.Id().AsInt()};
        std::vector<mgp::Node> frontier{start}, next;
        int64_t edges = 0;
        for (int64_t h = 0; h < hops && !frontier.empty(); ++h) {
            next.clear();
            for (const auto& node : frontier) {
                for (const auto& rel : node.OutRelationships()) {
                    ++edges;
                    if (rel.Type() != "transfer") continue;
                    const int64_t t = rel.GetProperty("createTime").ValueInt();
                    if (t <= t0 || t >= t1) continue;
                    mgp::Node to = rel.To();
                    if (seen.insert(to.Id().AsInt()).second) next.push_back(to);
                }
            }
            frontier.swap(next);
        }
        const int64_t micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();

        auto record = mgp::RecordFactory(result).NewRecord();
        record.Insert("reached", (int64_t)seen.size() - 1);
        record.Insert("edges", edges);
        record.Insert("micros", micros);
    } catch (const std::exception& e) {
        mgp::result_set_error_msg(result, e.what());
    }
}

}  // namespace

extern "C" int mgp_init_module(struct mgp_module* module, struct mgp_memory* memory) {
    try {
        mgp::MemoryDispatcherGuard guard(memory);
        mgp::AddProcedure(Out, "out", mgp::ProcedureType::Read,
                          {mgp::Parameter("start", mgp::Type::Node), mgp::Parameter("hops", mgp::Type::Int),
                           mgp::Parameter("t0", mgp::Type::Int), mgp::Parameter("t1", mgp::Type::Int)},
                          {mgp::Return("reached", mgp::Type::Int), mgp::Return("edges", mgp::Type::Int),
                           mgp::Return("micros", mgp::Type::Int)},
                          module, memory);
    } catch (const std::exception&) {
        return 1;
    }
    return 0;
}

extern "C" int mgp_shutdown_module() { return 0; }
