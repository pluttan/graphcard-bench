// gpc: Memgraph module with the T4 model in place of the card. It mirrors the transfer edges into
// a T4 structure and walks them the way the cores of the card would, then predicts the time of the
// card from the cycles of the RTL simulation.
//
//   CALL gpc.load() YIELD edges, accounts, words, micros
//       mirror every :transfer edge; the key is the account (as many bits as the accounts need),
//       the time from the first edge (37 bits, ms) and a number among the edges of one account in
//       one ms (the bits left of 64); the value is the account the edge goes to. Accounts are the
//       dense property `id`.
//   CALL gpc.khop(start, hops, t0, t1) YIELD reached, edges, ngr, nextc, card_ns, host_ns
//       breadth-first walk over transfers with t0 < createTime < t1, as Cypher *BFS 1..hops with
//       that filter: reached is the number of accounts found. The walk of one account is NGR of
//       the window start, then NEXTC of every answer until the window ends.
//   CALL gpc.khop_batch(jobs, hops, threads) YIELD reached, edges, ngr, nextc, card_ns, host_ns
//       the same walk for every job [account id, t0, t1] of the list at once: all walks move one hop
//       together, the accounts of all their frontiers share the cores, and the host merges the
//       answers of the jobs in `threads` threads. reached is the list of accounts found per job.
//
// The predicted time of the card (card_ns), per hop of the walk:
//   - the frontier goes to the cores by account % CORES, each core walks its accounts one by one;
//   - an account costs NGR_CYC, and every NEXTC costs the cycles of the cpe64 sw_kernel per edge
//     (make gpc-edges at DDR latency 30, results/next-cont/gpc-edges.txt) by the degree of the account;
//   - the hop lasts as long as the busiest core, plus PCIE_NS for the round trip and 8 bytes per
//     account sent and per edge returned at PCIE_GBS.
// host_ns is the real time this module spends to merge the answers: the visited set and the frontier.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <mgp.hpp>

#include "t4store.hpp"

namespace {

constexpr int CORES = 80;
constexpr double MHZ = 200.0;
constexpr double NGR_CYC = 180.0;          // NGR of a window start, make bench-edges at DDR latency 30
constexpr double PCIE_NS = 2000.0;         // one round trip host - card - host (assumed)
constexpr double PCIE_GBS = 10.0;          // XDMA throughput (assumed)
constexpr uint64_t TIME_BITS = 37;
uint64_t seq_bits = 8;  // 64 bits less the account and the time; set by gpc.load

// Cycles of one edge through the sw_kernel: NEXTC on the SPE plus the loop of the C program.
double edge_cycles(uint32_t degree) { return degree <= 15 ? 91.0 : degree <= 150 ? 109.0 : 91.0; }

std::mutex mu;
t4::T4Store* store = nullptr;
uint64_t tmin = 0, tmax = 0;
std::vector<uint32_t> degree;  // transfer edges out of every account

Key key_of(uint64_t acc, uint64_t t, uint64_t seq) {
    return (Key)(acc << (TIME_BITS + seq_bits) | (t - tmin) << seq_bits | seq);
}

int64_t prop_int(const mgp::Value& v) { return v.IsInt() ? v.ValueInt() : (int64_t)v.ValueDouble(); }

void Load(mgp_list*, mgp_graph* memgraph_graph, mgp_result* result, mgp_memory* memory) {
    try {
        mgp::MemoryDispatcherGuard guard(memory);
        auto begin = std::chrono::steady_clock::now();
        mgp::Graph graph(memgraph_graph);
        struct E { uint64_t from, to, t; };
        std::vector<E> es;
        uint64_t accounts = 0;
        for (const auto& n : graph.Nodes()) accounts = std::max<uint64_t>(accounts, prop_int(n.GetProperty("id")) + 1);
        for (const auto& r : graph.Relationships()) {
            if (r.Type() != "transfer") continue;
            es.push_back({(uint64_t)prop_int(r.From().GetProperty("id")), (uint64_t)prop_int(r.To().GetProperty("id")),
                          (uint64_t)prop_int(r.GetProperty("createTime"))});
        }
        uint64_t acc_bits = 1;
        while (acc_bits < 64 && (1ull << acc_bits) < accounts) ++acc_bits;
        if (acc_bits + TIME_BITS + 3 > 64) throw std::runtime_error("too many accounts for a 64-bit key");
        std::sort(es.begin(), es.end(), [](const E& a, const E& b) { return a.from != b.from ? a.from < b.from : a.t < b.t; });
        uint64_t lo = ~0ull, hi = 0;
        for (const E& e : es) lo = std::min(lo, e.t), hi = std::max(hi, e.t);
        if (!es.empty() && hi - lo >= 1ull << TIME_BITS) throw std::runtime_error("time range over 37 bits");
        std::lock_guard<std::mutex> lock(mu);
        tmin = lo, tmax = hi;
        seq_bits = 64 - acc_bits - TIME_BITS;
        degree.assign(accounts, 0);
        Pairs pairs;
        pairs.reserve(es.size());
        uint64_t seq = 0, dropped = 0;
        Key prev = ~(Key)0;
        for (const E& e : es) {
            Key base = key_of(e.from, e.t, 0);
            seq = base == prev ? seq + 1 : 0;
            prev = base;
            if (seq >= 1ull << seq_bits) { ++dropped; continue; }
            pairs.push_back({base | seq, e.to});
            ++degree[e.from];
        }
        if (dropped) throw std::runtime_error("too many edges of one account in one ms for the key");
        delete store;
        store = new t4::T4Store();
        store->assign(1, pairs);
        int64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        auto rec = mgp::RecordFactory(result).NewRecord();
        rec.Insert("edges", (int64_t)pairs.size());
        rec.Insert("accounts", (int64_t)accounts);
        rec.Insert("words", (int64_t)store->image().size());
        rec.Insert("micros", micros);
    } catch (const std::exception& e) {
        mgp::result_set_error_msg(result, e.what());
    }
}

void Khop(mgp_list* args, mgp_graph*, mgp_result* result, mgp_memory* memory) {
    try {
        mgp::MemoryDispatcherGuard guard(memory);
        const auto a = mgp::List(args);
        const uint64_t start = (uint64_t)prop_int(a[0].ValueNode().GetProperty("id"));
        const int64_t hops = a[1].ValueInt(), t0 = a[2].ValueInt(), t1 = a[3].ValueInt();
        std::lock_guard<std::mutex> lock(mu);
        if (!store) throw std::runtime_error("call gpc.load() first");
        // The window (t0, t1) as keys: from the first ms after t0 to the last ms before t1.
        const int64_t lo_t = std::max<int64_t>(t0 + 1, (int64_t)tmin), hi_t = std::min<int64_t>(t1 - 1, (int64_t)tmax);
        int64_t ngr = 0, nextc = 0, edges = 0;
        double card_ns = 0, host_ns = 0;
        std::unordered_set<uint64_t> seen{start};
        std::vector<uint64_t> frontier{start}, next, found;
        for (int64_t h = 0; h < hops && !frontier.empty(); ++h) {
            std::array<double, CORES> busy{};
            found.clear();
            for (uint64_t acc : frontier) {
                double cyc = NGR_CYC;
                ++ngr;
                if (lo_t <= hi_t) {
                    Key from = key_of(acc, lo_t, 0), end = key_of(acc, hi_t + 1, 0);
                    Found got = from == 0 ? store->first(1) : store->above(1, from - 1);
                    while (got && got->first < end) {
                        found.push_back(got->second);
                        got = store->above(1, got->first);
                        ++nextc, ++edges;
                        cyc += edge_cycles(degree[acc]);
                    }
                }
                busy[acc % CORES] += cyc;
            }
            card_ns += *std::max_element(busy.begin(), busy.end()) * 1000.0 / MHZ + PCIE_NS +
                       8.0 * (frontier.size() + found.size()) / PCIE_GBS;
            auto t = std::chrono::steady_clock::now();
            next.clear();
            for (uint64_t to : found)
                if (seen.insert(to).second) next.push_back(to);
            frontier.swap(next);
            host_ns += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t).count();
        }
        auto rec = mgp::RecordFactory(result).NewRecord();
        rec.Insert("reached", (int64_t)seen.size() - 1);
        rec.Insert("edges", edges);
        rec.Insert("ngr", ngr);
        rec.Insert("nextc", nextc);
        rec.Insert("card_ns", (int64_t)card_ns);
        rec.Insert("host_ns", (int64_t)host_ns);
    } catch (const std::exception& e) {
        mgp::result_set_error_msg(result, e.what());
    }
}

// One walk of a batch: its start, its window as times, the accounts seen and the frontier.
struct Walk {
    uint64_t start = 0;
    int64_t lo_t = 0, hi_t = -1;
    std::unordered_set<uint64_t> seen;
    std::vector<uint64_t> frontier, next, found;
};

void KhopBatch(mgp_list* args, mgp_graph*, mgp_result* result, mgp_memory* memory) {
    try {
        mgp::MemoryDispatcherGuard guard(memory);
        const auto a = mgp::List(args);
        const auto list = a[0].ValueList();
        const int64_t hops = a[1].ValueInt();
        const int threads = (int)std::max<int64_t>(1, a[2].ValueInt());
        std::lock_guard<std::mutex> lock(mu);
        if (!store) throw std::runtime_error("call gpc.load() first");
        std::vector<Walk> walks(list.Size());
        for (size_t i = 0; i < walks.size(); ++i) {
            const auto j = list[i].ValueList();
            Walk& w = walks[i];
            w.start = (uint64_t)j[0].ValueInt();
            w.lo_t = std::max<int64_t>(j[1].ValueInt() + 1, (int64_t)tmin);
            w.hi_t = std::min<int64_t>(j[2].ValueInt() - 1, (int64_t)tmax);
            w.seen.insert(w.start);
            w.frontier.push_back(w.start);
        }
        int64_t ngr = 0, nextc = 0, edges = 0;
        double card_ns = 0, host_ns = 0;
        for (int64_t h = 0; h < hops; ++h) {
            // The cores: every account of every frontier, walked by the core of its account.
            std::array<double, CORES> busy{};
            uint64_t sent = 0, back = 0;
            for (Walk& w : walks) {
                w.found.clear();
                for (uint64_t acc : w.frontier) {
                    double cyc = NGR_CYC;
                    ++ngr, ++sent;
                    if (w.lo_t <= w.hi_t) {
                        Key from = key_of(acc, w.lo_t, 0), end = key_of(acc, w.hi_t + 1, 0);
                        Found got = from == 0 ? store->first(1) : store->above(1, from - 1);
                        while (got && got->first < end) {
                            w.found.push_back(got->second);
                            got = store->above(1, got->first);
                            ++nextc, ++edges, ++back;
                            cyc += edge_cycles(degree[acc]);
                        }
                    }
                    busy[acc % CORES] += cyc;
                }
            }
            if (sent == 0) break;
            card_ns += *std::max_element(busy.begin(), busy.end()) * 1000.0 / MHZ + PCIE_NS + 8.0 * (sent + back) / PCIE_GBS;
            // The host: the answers of every walk into its seen set and its next frontier.
            auto t = std::chrono::steady_clock::now();
            std::vector<std::thread> pool;
            for (int k = 0; k < threads; ++k)
                pool.emplace_back([&walks, k, threads] {
                    for (size_t i = k; i < walks.size(); i += threads) {
                        Walk& w = walks[i];
                        w.next.clear();
                        for (uint64_t to : w.found)
                            if (w.seen.insert(to).second) w.next.push_back(to);
                        w.frontier.swap(w.next);
                    }
                });
            for (auto& th : pool) th.join();
            host_ns += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t).count();
        }
        mgp::List reached(walks.size());
        for (const Walk& w : walks) reached.Append(mgp::Value((int64_t)w.seen.size() - 1));
        auto rec = mgp::RecordFactory(result).NewRecord();
        rec.Insert("reached", reached);
        rec.Insert("edges", edges);
        rec.Insert("ngr", ngr);
        rec.Insert("nextc", nextc);
        rec.Insert("card_ns", (int64_t)card_ns);
        rec.Insert("host_ns", (int64_t)host_ns);
    } catch (const std::exception& e) {
        mgp::result_set_error_msg(result, e.what());
    }
}

}  // namespace

extern "C" int mgp_init_module(struct mgp_module* module, struct mgp_memory* memory) {
    try {
        mgp::MemoryDispatcherGuard guard(memory);
        mgp::AddProcedure(Load, "load", mgp::ProcedureType::Read, {},
                          {mgp::Return("edges", mgp::Type::Int), mgp::Return("accounts", mgp::Type::Int),
                           mgp::Return("words", mgp::Type::Int), mgp::Return("micros", mgp::Type::Int)},
                          module, memory);
        mgp::AddProcedure(Khop, "khop", mgp::ProcedureType::Read,
                          {mgp::Parameter("start", mgp::Type::Node), mgp::Parameter("hops", mgp::Type::Int),
                           mgp::Parameter("t0", mgp::Type::Int), mgp::Parameter("t1", mgp::Type::Int)},
                          {mgp::Return("reached", mgp::Type::Int), mgp::Return("edges", mgp::Type::Int),
                           mgp::Return("ngr", mgp::Type::Int), mgp::Return("nextc", mgp::Type::Int),
                           mgp::Return("card_ns", mgp::Type::Int), mgp::Return("host_ns", mgp::Type::Int)},
                          module, memory);
        mgp::AddProcedure(KhopBatch, "khop_batch", mgp::ProcedureType::Read,
                          {mgp::Parameter("jobs", {mgp::Type::List, mgp::Type::Any}), mgp::Parameter("hops", mgp::Type::Int),
                           mgp::Parameter("threads", mgp::Type::Int)},
                          {mgp::Return("reached", {mgp::Type::List, mgp::Type::Int}), mgp::Return("edges", mgp::Type::Int),
                           mgp::Return("ngr", mgp::Type::Int), mgp::Return("nextc", mgp::Type::Int),
                           mgp::Return("card_ns", mgp::Type::Int), mgp::Return("host_ns", mgp::Type::Int)},
                          module, memory);
    } catch (const std::exception&) {
        return 1;
    }
    return 0;
}

extern "C" int mgp_shutdown_module() {
    delete store;
    store = nullptr;
    return 0;
}
