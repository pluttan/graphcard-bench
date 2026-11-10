// DISC command semantics as the real lnh64 answers them (see golden/*.gpc).
// Disc<Store> holds the rules once; a Store only keeps ordered pairs per structure.
#pragma once
#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <vector>

constexpr int kStructs = 8;  // structure 0 exists but stays empty, as in lnh64

// A key is the whole 128-bit key register of the lnh64 API: high half, then low half.
using Key = unsigned __int128;

inline uint64_t key_hi(Key k) { return (uint64_t)(k >> 64); }
inline uint64_t key_lo(Key k) { return (uint64_t)k; }
inline Key make_key(uint64_t hi, uint64_t lo) { return (Key)hi << 64 | lo; }

struct Answer {
    bool ok = false;
    Key key = 0;
    uint64_t val = 0;
};

using Pair = std::pair<Key, uint64_t>;
using Pairs = std::vector<Pair>;
using Found = std::optional<Pair>;

// Store interface: clear, put, erase, find, above, below, first, last, size, pairs, assign, squeeze.
template <class Store>
class Disc {
public:
    Store& store() { return st_; }

    void reset() {
        for (int s = 0; s < kStructs; ++s) st_.clear(s);
    }

    Answer ins(int s, Key k, uint64_t v) {
        if (s == 0) return {};  // structure 0 refuses keys
        st_.put(s, k, v);       // an existing key gets the new value
        return {true, k, v};
    }

    Answer del(int s, Key k) { return {st_.erase(s, k), k, 0}; }
    Answer srch(int s, Key k) { return from(st_.find(s, k)); }

    // NEXT and PREV need the key itself in the structure, NSM and NGR do not.
    Answer next(int s, Key k) { return st_.find(s, k) ? from(st_.above(s, k)) : Answer{}; }
    Answer prev(int s, Key k) { return st_.find(s, k) ? from(st_.below(s, k)) : Answer{}; }
    Answer nsm(int s, Key k) { return from(st_.below(s, k)); }
    Answer ngr(int s, Key k) { return from(st_.above(s, k)); }

    Answer min(int s) { return from(st_.first(s)); }
    Answer max(int s) { return from(st_.last(s)); }
    Answer cnt(int s) { return {true, st_.size(s), 0}; }

    // Set operations take the value from A when a key is in both structures.
    // lnh64 refuses (ok = 0, R untouched) when R is not empty or an operand is empty.
    Answer op_and(int a, int b, int r) {
        return binary(a, b, r, [](const Pair* x, const Pair* y) { return x && y ? x : nullptr; });
    }

    Answer op_or(int a, int b, int r) {
        return binary(a, b, r, [](const Pair* x, const Pair* y) { return x ? x : y; });
    }

    Answer op_not(int a, int b, int r) {
        return binary(a, b, r, [](const Pair* x, const Pair* y) { return x && !y ? x : nullptr; });
    }

    // Slices: keys of A below or above a bound; GRLS keeps keys strictly between low and high.
    // GR with no greater key and GRLS with low above high also refuse.
    Answer slice(int a, int r, Key low, bool low_in, Key high, bool high_in, bool has_low,
                 bool has_high) {
        if (!ready(r, a)) return {};
        if (has_low && !low_in && !has_high && !st_.above(a, low)) return {};
        if (has_low && has_high && low > high) return {};
        Pairs out;
        for (const Pair& p : st_.pairs(a)) {
            Key k = p.first;
            if (has_low && (k < low || (k == low && !low_in))) continue;
            if (has_high && (k > high || (k == high && !high_in))) continue;
            out.push_back(p);
        }
        st_.assign(r, out);
        return {true, 0, 0};
    }

    Answer dels(int s) {
        st_.clear(s);
        return {true, 0, 0};
    }

    Answer sq(int s) {
        st_.squeeze(s);
        return {true, 0, 0};
    }

    Pairs dump(int s) { return st_.pairs(s); }

private:
    static Answer from(const Found& f) { return f ? Answer{true, f->first, f->second} : Answer{}; }

    bool ready(int r, int a) { return r != 0 && st_.size(r) == 0 && st_.size(a) != 0; }

    // Merge of two sorted streams; pick decides which pair, if any, goes into R.
    template <class Pick>
    Answer binary(int a, int b, int r, Pick pick) {
        if (!ready(r, a) || st_.size(b) == 0) return {};
        Pairs x = st_.pairs(a), y = st_.pairs(b), out;
        size_t i = 0, j = 0;
        while (i < x.size() || j < y.size()) {
            const Pair* px = i < x.size() && (j == y.size() || x[i].first <= y[j].first) ? &x[i] : nullptr;
            const Pair* py = j < y.size() && (i == x.size() || y[j].first <= x[i].first) ? &y[j] : nullptr;
            if (const Pair* p = pick(px, py)) out.push_back(*p);
            i += px != nullptr;
            j += py != nullptr;
        }
        st_.assign(r, out);
        return {true, 0, 0};
    }

    Store st_;
};

// ==========================
// ===  Reference store   ===
// ==========================

class MapStore {
public:
    void clear(int s) { m_[s].clear(); }
    void put(int s, Key k, uint64_t v) { m_[s][k] = v; }
    bool erase(int s, Key k) { return m_[s].erase(k) > 0; }
    size_t size(int s) const { return m_[s].size(); }
    void squeeze(int) {}

    Found find(int s, Key k) const {
        auto it = m_[s].find(k);
        return it == m_[s].end() ? Found{} : Found{*it};
    }

    Found above(int s, Key k) const {
        auto it = m_[s].upper_bound(k);
        return it == m_[s].end() ? Found{} : Found{*it};
    }

    Found below(int s, Key k) const {
        auto it = m_[s].lower_bound(k);
        return it == m_[s].begin() ? Found{} : Found{*std::prev(it)};
    }

    Found first(int s) const { return m_[s].empty() ? Found{} : Found{*m_[s].begin()}; }
    Found last(int s) const { return m_[s].empty() ? Found{} : Found{*m_[s].rbegin()}; }
    Pairs pairs(int s) const { return Pairs(m_[s].begin(), m_[s].end()); }
    void assign(int s, const Pairs& p) { m_[s] = std::map<Key, uint64_t>(p.begin(), p.end()); }

private:
    std::array<std::map<Key, uint64_t>, kStructs> m_;
};

using MapDisc = Disc<MapStore>;
