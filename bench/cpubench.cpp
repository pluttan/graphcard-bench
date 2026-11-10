// cpubench: the DISC commands of the GPC bench on an ordinary processor, with the same keys mix(i).
//
//   cpubench N [threads [par]]   N keys per structure; threads for the parallel part, 0 = all;
//                                par: only the parallel part
//
// Three structures stand for the SPE structure:
//   map    std::map, ordered and dynamic, like the B+ tree of the SPE;
//   sorted a sorted array of pairs, built once; the best case for search and for the set commands;
//   hash   open addressing, exact search only.
// Every line: command, structure, N, threads, nanoseconds per key, millions of keys per second.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

using u64 = uint64_t;
using Pair = std::pair<u64, u64>;
using Clock = std::chrono::steady_clock;

static u64 mix(u64 i) { return i * 0x9E3779B97F4A7C15ull; }

static double seconds(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

static void report(const char* cmd, const char* str, u64 n, int threads, u64 keys, double s) {
    printf("%-8s %-7s %10llu %3d %9.2f ns %9.2f Mkeys/s\n", cmd, str, (unsigned long long)n, threads,
           s * 1e9 / (double)keys, (double)keys / s / 1e6);
    fflush(stdout);
}

// ==========================
// ===  Hash table        ===
// ==========================

// Linear probing over a power of two, key 0 marks an empty slot (mix(0) = 0 is kept aside).
struct Hash {
    std::vector<Pair> slot;
    u64 mask;
    bool has_zero = false;
    u64 zero_val = 0;
    explicit Hash(u64 n) {
        u64 cap = 1;
        while (cap < n * 2) cap <<= 1;
        slot.assign(cap, {0, 0});
        mask = cap - 1;
    }
    static u64 h(u64 k) { return (k ^ (k >> 29)) * 0xBF58476D1CE4E5B9ull; }
    void ins(u64 k, u64 v) {
        if (!k) { has_zero = true, zero_val = v; return; }
        for (u64 i = h(k) & mask;; i = (i + 1) & mask)
            if (slot[i].first == 0 || slot[i].first == k) { slot[i] = {k, v}; return; }
    }
    bool find(u64 k, u64& v) const {
        if (!k) { v = zero_val; return has_zero; }
        for (u64 i = h(k) & mask;; i = (i + 1) & mask) {
            if (slot[i].first == k) { v = slot[i].second; return true; }
            if (slot[i].first == 0) return false;
        }
    }
};

// ==========================
// ===  Searches          ===
// ==========================

static bool sorted_find(const std::vector<Pair>& a, u64 k, u64& v) {
    auto it = std::lower_bound(a.begin(), a.end(), k, [](const Pair& p, u64 x) { return p.first < x; });
    if (it == a.end() || it->first != k) return false;
    v = it->second;
    return true;
}

static bool sorted_next(const std::vector<Pair>& a, u64 k, u64& v) {
    auto it = std::upper_bound(a.begin(), a.end(), k, [](u64 x, const Pair& p) { return x < p.first; });
    if (it == a.end()) return false;
    v = it->first;
    return true;
}

// Queries in a fixed scattered order, made before the clock starts.
static std::vector<u64> queries(u64 n, u64 q, bool hit) {
    std::vector<u64> out(q);
    u64 s = 12345;
    for (u64 i = 0; i < q; ++i) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        out[i] = hit ? mix((s >> 11) % n) : mix(n + (s >> 11) % n);
    }
    return out;
}

template <class F>
static double run_queries(const std::vector<u64>& q, F f) {
    u64 sink = 0;
    auto t0 = Clock::now();
    for (u64 k : q) {
        u64 v = 0;
        if (f(k, v)) sink += v;
    }
    double s = seconds(t0);
    if (sink == 42) printf("#");
    return s;
}

// All threads search one shared structure. Every thread walks all the queries from its own starting
// point, so each thread works long enough for its start-up to vanish; the clock starts when all are ready.
template <class F>
static double run_parallel(const std::vector<u64>& q, int threads, F f) {
    std::vector<std::thread> pool;
    std::atomic<u64> sink{0};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            u64 n = q.size(), first = n * t / threads, s = 0;
            ++ready;
            while (!go) {}
            for (u64 j = 0; j < n; ++j) {
                u64 i = first + j < n ? first + j : first + j - n, v = 0;
                if (f(q[i], v)) s += v;
            }
            sink += s;
        });
    while (ready < threads) {}
    auto t0 = Clock::now();
    go = true;
    for (auto& th : pool) th.join();
    return seconds(t0);
}

// ==========================
// ===  Set commands      ===
// ==========================

static std::vector<Pair> sorted_range(u64 first, u64 n) {
    std::vector<Pair> a(n);
    for (u64 i = 0; i < n; ++i) a[i] = {mix(first + i), first + i};
    std::sort(a.begin(), a.end());
    return a;
}

static bool key_less(const Pair& x, const Pair& y) { return x.first < y.first; }

static void set_commands(u64 n) {
    std::vector<Pair> a = sorted_range(0, n), b = sorted_range(n / 2, n), r;
    r.reserve(2 * n);
    auto time = [&](const char* name, auto f) {
        double best = 1e9;
        for (int rep = 0; rep < 5; ++rep) {
            r.clear();
            auto t0 = Clock::now();
            f();
            best = std::min(best, seconds(t0));
        }
        report(name, "sorted", n, 1, 2 * n, best);  // per input key of both operands, as on the GPC
    };
    time("AND", [&] { std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(r), key_less); });
    time("OR", [&] { std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(r), key_less); });
    time("NOT", [&] { std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(r), key_less); });

    // Slices: per key of the result, as GR and GRLS on the GPC.
    double best = 1e9;
    u64 got = 0;
    for (int rep = 0; rep < 5; ++rep) {
        r.clear();
        auto t0 = Clock::now();
        auto lo = std::upper_bound(a.begin(), a.end(), Pair{0x8000000000000000ull, ~0ull}, key_less);
        r.assign(lo, a.end());
        best = std::min(best, seconds(t0));
        got = r.size();
    }
    report("GR", "sorted", n, 1, got, best);

    // The same AND over two std::map, walking both in order.
    std::map<u64, u64> ma(a.begin(), a.end()), mb(b.begin(), b.end());
    best = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
        r.clear();
        auto t0 = Clock::now();
        std::set_intersection(ma.begin(), ma.end(), mb.begin(), mb.end(), std::back_inserter(r), key_less);
        best = std::min(best, seconds(t0));
    }
    report("AND", "map", n, 1, 2 * n, best);
}

// k threads, each with its own pair of sets: the CPU twin of k GPC cores, each doing its own AND.
// Results go to buffers written in advance, so no thread touches a cache line of another.
// Each thread repeats its AND 10 times to work long enough.
static void set_parallel(u64 n, int threads) {
    std::vector<std::vector<Pair>> a(threads), b(threads), r(threads);
    std::vector<std::thread> build;
    for (int t = 0; t < threads; ++t)  // each thread fills its own operands: their pages land on its NUMA node
        build.emplace_back([&, t] {
            a[t] = sorted_range((u64)t * 4 * n, n);
            b[t] = sorted_range((u64)t * 4 * n + n / 2, n);
            r[t].assign(n, {1, 1});
        });
    for (auto& th : build) th.join();
    double best = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
        std::vector<std::thread> pool;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                ++ready;
                while (!go) {}
                for (int k = 0; k < 10; ++k)
                    std::set_intersection(a[t].begin(), a[t].end(), b[t].begin(), b[t].end(), r[t].data(), key_less);
            });
        while (ready < threads) {}
        auto t0 = Clock::now();
        go = true;
        for (auto& th : pool) th.join();
        best = std::min(best, seconds(t0));
    }
    report("AND", "sorted", n, threads, 2 * n * threads * 10, best);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s N [threads]\n", argv[0]);
        return 1;
    }
    u64 n = strtoull(argv[1], nullptr, 0);
    int threads = argc > 2 ? atoi(argv[2]) : 0;
    if (threads <= 0) threads = (int)std::thread::hardware_concurrency();
    u64 q = std::max<u64>(1000000, std::min<u64>(n, 10000000));
    bool par_only = argc > 3 && std::string(argv[3]) == "par";

    // INS: keys in the scattered order mix(i), as FILL does on the CPE.
    std::map<u64, u64> m;
    auto t0 = Clock::now();
    for (u64 i = 0; i < n; ++i) m.emplace(mix(i), i);
    report("INS", "map", n, 1, n, seconds(t0));

    Hash h(n);
    t0 = Clock::now();
    for (u64 i = 0; i < n; ++i) h.ins(mix(i), i);
    report("INS", "hash", n, 1, n, seconds(t0));

    std::vector<Pair> s(n);
    t0 = Clock::now();
    for (u64 i = 0; i < n; ++i) s[i] = {mix(i), i};
    std::sort(s.begin(), s.end());
    report("BUILD", "sorted", n, 1, n, seconds(t0));

    // Point commands, one thread.
    std::vector<u64> hit = queries(n, q, true), miss = queries(n, q, false);
    auto map_find0 = [&](u64 k, u64& v) { auto it = m.find(k); if (it == m.end()) return false; v = it->second; return true; };
    if (par_only) {
        auto srt_find0 = [&](u64 k, u64& v) { return sorted_find(s, k, v); };
        auto hsh_find0 = [&](u64 k, u64& v) { return h.find(k, v); };
        report("SRCH", "map", n, threads, q * threads, run_parallel(hit, threads, map_find0));
        report("SRCH", "sorted", n, threads, q * threads, run_parallel(hit, threads, srt_find0));
        report("SRCH", "hash", n, threads, q * threads, run_parallel(hit, threads, hsh_find0));
        set_parallel(std::min<u64>(n, 1000000), threads);
        return 0;
    }
    auto map_find = [&](u64 k, u64& v) { auto it = m.find(k); if (it == m.end()) return false; v = it->second; return true; };
    auto map_next = [&](u64 k, u64& v) { auto it = m.upper_bound(k); if (it == m.end()) return false; v = it->first; return true; };
    auto srt_find = [&](u64 k, u64& v) { return sorted_find(s, k, v); };
    auto srt_next = [&](u64 k, u64& v) { return sorted_next(s, k, v); };
    auto hsh_find = [&](u64 k, u64& v) { return h.find(k, v); };
    report("SRCH", "map", n, 1, q, run_queries(hit, map_find));
    report("SRCH", "sorted", n, 1, q, run_queries(hit, srt_find));
    report("SRCH", "hash", n, 1, q, run_queries(hit, hsh_find));
    report("SRCHmiss", "map", n, 1, q, run_queries(miss, map_find));
    report("SRCHmiss", "sorted", n, 1, q, run_queries(miss, srt_find));
    report("SRCHmiss", "hash", n, 1, q, run_queries(miss, hsh_find));
    report("NEXT", "map", n, 1, q, run_queries(hit, map_next));
    report("NEXT", "sorted", n, 1, q, run_queries(hit, srt_next));

    // Point commands, all threads on one shared structure.
    report("SRCH", "map", n, threads, q * threads, run_parallel(hit, threads, map_find));
    report("SRCH", "sorted", n, threads, q * threads, run_parallel(hit, threads, srt_find));
    report("SRCH", "hash", n, threads, q * threads, run_parallel(hit, threads, hsh_find));

    m.clear();
    s.clear();
    s.shrink_to_fit();
    set_commands(n);
    set_parallel(std::min<u64>(n, 1000000), threads);
    return 0;
}
