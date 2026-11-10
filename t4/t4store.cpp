#include "t4store.hpp"

#include <algorithm>
#include <stdexcept>

namespace t4 {

namespace {

// Starts of the runs of keys that share byte p, closed by hi.
std::vector<size_t> groups(const Pairs& v, size_t lo, size_t hi, int p) {
    std::vector<size_t> g{lo};
    for (size_t i = lo + 1; i < hi; ++i)
        if (byte_at(v[i].first, p) != byte_at(v[i - 1].first, p)) g.push_back(i);
    g.push_back(hi);
    return g;
}

bool key_less(const Pair& a, const Pair& b) { return a.first < b.first; }

}  // namespace

// ==========================
// ===  Encoding          ===
// ==========================

// One high half for the whole pack, zero or in a word of its own, else a high half per pair.
uint64_t pack_flags(const Pairs& p) {
    for (const Pair& x : p)
        if (key_hi(x.first) != key_hi(p.front().first)) return kFlagW;
    return p.empty() || key_hi(p.front().first) == 0 ? 0 : kFlagH;
}

std::vector<uint64_t> encode_pack(const Pairs& p) {
    uint64_t f = pack_flags(p);
    std::vector<uint64_t> w{head(PACK, (int)p.size(), 0, f)};
    if (f & kFlagH) w.push_back(key_hi(p.front().first));
    for (auto [k, v] : p) {
        if (f & kFlagW) w.push_back(key_hi(k));
        w.push_back(key_lo(k));
        w.push_back(v);
    }
    return w;
}

std::vector<uint64_t> encode_branches(bool values, int p, Key pre, const std::vector<int>& bytes,
                                      const std::vector<uint64_t>& entries) {
    int c = (int)bytes.size();
    bool bitmap = c > kListMax;
    Type t = values ? (bitmap ? VALUES_BITMAP : VALUES_LIST) : (bitmap ? INNER_BITMAP : INNER_LIST);
    std::vector<uint64_t> w{head(t, p, c, pre_wide(p, pre) ? kFlagH : 0)};
    if (p <= 8) {
        w.push_back(key_hi(pre));
    } else {
        if (pre_wide(p, pre)) w.push_back(key_hi(pre));
        w.push_back(key_lo(pre));
    }
    std::vector<uint64_t> set(set_words(c), 0);
    for (int i = 0; i < c; ++i) {
        int b = bytes[i];
        if (bitmap) set[b / 64] |= 1ull << (b % 64);
        else set[i / 8] |= (uint64_t)b << (8 * (i % 8));
    }
    w.insert(w.end(), set.begin(), set.end());
    if (values) {
        w.insert(w.end(), entries.begin(), entries.end());
        return w;
    }
    std::vector<uint64_t> a(addr_words(c), 0);
    for (int i = 0; i < c; ++i) a[i / 2] |= (entries[i] & 0xffffffffull) << (32 * (i % 2));
    w.insert(w.end(), a.begin(), a.end());
    return w;
}

// ==========================
// ===  Memory            ===
// ==========================

uint32_t T4Store::alloc(size_t words) {
    size_t at = mem_.size();
    if (at + words >= (1ull << 32)) throw std::runtime_error("T4 memory image over 32 GB");
    mem_.resize(at + words, 0);
    return (uint32_t)at;
}

uint32_t T4Store::store(const std::vector<uint64_t>& w) {
    uint32_t at = alloc(w.size());
    store_at(at, w);
    return at;
}

void T4Store::store_at(uint32_t at, const std::vector<uint64_t>& w) {
    std::copy(w.begin(), w.end(), mem_.begin() + at);
    stats_.words_written += w.size();
}

void T4Store::set_entry(const Node& n, int i, uint64_t e) {
    stats_.words_written += 1;
    if (is_values(n.type)) {
        mem_[n.entry_at + i] = e;
        return;
    }
    uint64_t& w = mem_[n.entry_at + i / 2];
    int shift = 32 * (i % 2);
    w = (w & ~(0xffffffffull << shift)) | (e & 0xffffffffull) << shift;
}

Node T4Store::load(uint32_t at) {
    stats_.node_reads += 1;
    Node n;
    n.at = at;
    uint64_t h = mem_[at];
    n.type = type_of(h);
    n.flags = h & (kFlagH | kFlagW);
    if (n.type == PACK) {
        uint64_t hi = has_h(h) ? mem_[at + 1] : 0;
        n.entry_at = at + 1 + (has_h(h) ? 1 : 0);
        for (int i = 0; i < pn_of(h); ++i) {
            if (has_w(h)) {
                uint32_t e = n.entry_at + 3 * i;
                n.pairs.push_back({make_key(mem_[e], mem_[e + 1]), mem_[e + 2]});
            } else {
                uint32_t e = n.entry_at + 2 * i;
                n.pairs.push_back({make_key(hi, mem_[e]), mem_[e + 1]});
            }
        }
        return n;
    }
    n.p = pn_of(h);
    if (n.p <= 8) n.pre = make_key(mem_[at + 1], 0);
    else n.pre = has_h(h) ? make_key(mem_[at + 1], mem_[at + 2]) : make_key(0, mem_[at + 1]);
    n.set_at = at + 1 + pre_words(n.p, n.pre);
    int c = count_of(h);
    const uint64_t* set = &mem_[n.set_at];
    if (is_bitmap(n.type)) {
        for (int b = 0; b < 256; ++b)
            if (has_bit(set, b)) n.bytes.push_back(b);
    } else {
        for (int i = 0; i < c; ++i) n.bytes.push_back((int)((set[i / 8] >> (8 * (i % 8))) & 0xff));
    }
    n.entry_at = n.set_at + set_words(c);
    for (int i = 0; i < c; ++i)
        n.entries.push_back(is_values(n.type) ? mem_[n.entry_at + i]
                                              : (mem_[n.entry_at + i / 2] >> (32 * (i % 2))) & 0xffffffffull);
    return n;
}

// The bitmap form finds the entry by counting set bits below the byte, as the hardware does.
int T4Store::find_branch(const Node& n, int b) const {
    if (is_bitmap(n.type)) {
        const uint64_t* set = &mem_[n.set_at];
        return has_bit(set, b) ? rank(set, b) : -1;
    }
    for (size_t i = 0; i < n.bytes.size(); ++i)
        if (n.bytes[i] == b) return (int)i;
    return -1;
}

// ==========================
// ===  Canonical build   ===
// ==========================

size_t T4Store::node_words(const Pairs& v, size_t lo, size_t hi) const {
    size_t n = hi - lo;
    if (n <= kPackMax) return pack_words((int)n, pack_flags(Pairs(v.begin() + lo, v.begin() + hi)));
    int p = first_diff(v[lo].first, v[hi - 1].first);
    if (p == kLast) return values_words((int)n, prefix(v[lo].first, p));
    return inner_words((int)groups(v, lo, hi, p).size() - 1, p, prefix(v[lo].first, p));
}

// Writes the node for v[lo, hi) at `at` and the children of an inner node into one block.
void T4Store::place(const Pairs& v, size_t lo, size_t hi, uint32_t at) {
    size_t n = hi - lo;
    if (n <= kPackMax) {
        store_at(at, encode_pack(Pairs(v.begin() + lo, v.begin() + hi)));
        return;
    }
    int p = first_diff(v[lo].first, v[hi - 1].first);
    Key pre = prefix(v[lo].first, p);
    std::vector<int> bytes;
    std::vector<uint64_t> entries;
    if (p == kLast) {
        for (size_t i = lo; i < hi; ++i) {
            bytes.push_back(byte_at(v[i].first, kLast));
            entries.push_back(v[i].second);
        }
        store_at(at, encode_branches(true, kLast, pre, bytes, entries));
        return;
    }
    std::vector<size_t> g = groups(v, lo, hi, p);
    std::vector<size_t> sizes;
    size_t total = 0;
    for (size_t i = 0; i + 1 < g.size(); ++i) total += sizes.emplace_back(node_words(v, g[i], g[i + 1]));
    uint32_t cur = alloc(total);
    for (size_t i = 0; i + 1 < g.size(); ++i) {
        bytes.push_back(byte_at(v[g[i]].first, p));
        entries.push_back(cur);
        cur += (uint32_t)sizes[i];
    }
    store_at(at, encode_branches(false, p, pre, bytes, entries));
    for (size_t i = 0; i + 1 < g.size(); ++i) place(v, g[i], g[i + 1], (uint32_t)entries[i]);
}

uint32_t T4Store::build(const Pairs& v) {
    if (v.empty()) return 0;
    uint32_t at = alloc(node_words(v, 0, v.size()));
    place(v, 0, v.size(), at);
    return at;
}

void T4Store::assign(int s, const Pairs& sorted) {
    root_[s] = build(sorted);
    size_[s] = sorted.size();
}

// ==========================
// ===  INS and DEL       ===
// ==========================

void T4Store::put(int s, Key k, uint64_t v) {
    bool added = false;
    if (root_[s]) {
        root_[s] = insert(root_[s], k, v, added);
    } else {
        root_[s] = store(encode_pack({{k, v}}));
        added = true;
    }
    size_[s] += added;
}

bool T4Store::erase(int s, Key k) {
    bool removed = false;
    if (root_[s]) root_[s] = remove(root_[s], k, removed);
    size_[s] -= removed;
    return removed;
}

// Returns the subtree address: the same one when nothing grew, a new one otherwise.
uint32_t T4Store::insert(uint32_t at, Key k, uint64_t v, bool& added) {
    Node n = load(at);
    if (n.type == PACK) {
        Pairs& p = n.pairs;
        auto it = std::lower_bound(p.begin(), p.end(), Pair{k, 0}, key_less);
        if (it != p.end() && it->first == k) {
            long i = it - p.begin();
            mem_[n.entry_at + (has_w(n.flags) ? 3 * i + 2 : 2 * i + 1)] = v;
            stats_.words_written += 1;
            return at;
        }
        added = true;
        p.insert(it, {k, v});
        return p.size() <= (size_t)kPackMax ? store(encode_pack(p)) : build(p);
    }
    int d = first_diff(k, n.pre);
    if (d < n.p) {  // the key leaves the prefix shared below this node
        added = true;
        uint32_t leaf = store(encode_pack({{k, v}}));
        std::vector<int> bytes{byte_at(k, d), byte_at(n.pre, d)};
        std::vector<uint64_t> entries{leaf, at};
        if (bytes[0] > bytes[1]) {
            std::swap(bytes[0], bytes[1]);
            std::swap(entries[0], entries[1]);
        }
        return store(encode_branches(false, d, prefix(k, d), bytes, entries));
    }
    int b = byte_at(k, n.p);
    int i = find_branch(n, b);
    if (i >= 0) {
        if (is_values(n.type)) {
            set_entry(n, i, v);
            return at;
        }
        uint32_t child = (uint32_t)n.entries[i];
        uint32_t nc = insert(child, k, v, added);
        if (nc != child) set_entry(n, i, nc);
        return at;
    }
    added = true;
    size_t pos = std::lower_bound(n.bytes.begin(), n.bytes.end(), b) - n.bytes.begin();
    n.bytes.insert(n.bytes.begin() + pos, b);
    n.entries.insert(n.entries.begin() + pos, is_values(n.type) ? v : store(encode_pack({{k, v}})));
    return store(encode_branches(is_values(n.type), n.p, n.pre, n.bytes, n.entries));
}

// Returns the subtree address, or 0 when the subtree became empty. A smaller node is
// rewritten in place, so DEL never needs free memory.
uint32_t T4Store::remove(uint32_t at, Key k, bool& removed) {
    Node n = load(at);
    if (n.type == PACK) {
        Pairs& p = n.pairs;
        auto it = std::lower_bound(p.begin(), p.end(), Pair{k, 0}, key_less);
        if (it == p.end() || it->first != k) return at;
        removed = true;
        if (p.size() == 1) return 0;
        p.erase(it);
        store_at(at, encode_pack(p));
        return at;
    }
    if (first_diff(k, n.pre) < n.p) return at;
    int i = find_branch(n, byte_at(k, n.p));
    if (i < 0) return at;
    if (is_inner(n.type)) {
        uint32_t child = (uint32_t)n.entries[i];
        uint32_t nc = remove(child, k, removed);
        if (nc == child) return at;
        if (nc != 0) {
            set_entry(n, i, nc);
            return at;
        }
    } else {
        removed = true;
    }
    if (n.bytes.size() == 1) return 0;
    n.bytes.erase(n.bytes.begin() + i);
    n.entries.erase(n.entries.begin() + i);
    store_at(at, encode_branches(is_values(n.type), n.p, n.pre, n.bytes, n.entries));
    return at;
}

// ==========================
// ===  Ordered queries   ===
// ==========================

Found T4Store::find(int s, Key k) {
    uint32_t at = root_[s];
    while (at) {
        Node n = load(at);
        if (n.type == PACK) {
            for (const Pair& p : n.pairs)
                if (p.first == k) return p;
            return {};
        }
        if (first_diff(k, n.pre) < n.p) return {};
        int i = find_branch(n, byte_at(k, n.p));
        if (i < 0) return {};
        if (is_values(n.type)) return Pair{k, n.entries[i]};
        at = (uint32_t)n.entries[i];
    }
    return {};
}

Found T4Store::first_in(uint32_t at) {
    Node n = load(at);
    if (n.type == PACK) return n.pairs.front();
    if (is_values(n.type)) return Pair{n.pre | (Key)n.bytes.front(), n.entries.front()};
    return first_in((uint32_t)n.entries.front());
}

Found T4Store::last_in(uint32_t at) {
    Node n = load(at);
    if (n.type == PACK) return n.pairs.back();
    if (is_values(n.type)) return Pair{n.pre | (Key)n.bytes.back(), n.entries.back()};
    return last_in((uint32_t)n.entries.back());
}

// Smallest key above k in the subtree.
Found T4Store::above(uint32_t at, Key k) {
    Node n = load(at);
    if (n.type == PACK) {
        for (const Pair& p : n.pairs)
            if (p.first > k) return p;
        return {};
    }
    int d = first_diff(k, n.pre);
    if (d < n.p) return byte_at(k, d) < byte_at(n.pre, d) ? first_in(at) : Found{};
    int b = byte_at(k, n.p);
    for (size_t i = 0; i < n.bytes.size(); ++i) {
        if (n.bytes[i] < b) continue;
        if (is_values(n.type)) {
            if (n.bytes[i] > b) return Pair{n.pre | (Key)n.bytes[i], n.entries[i]};
            continue;
        }
        if (n.bytes[i] > b) return first_in((uint32_t)n.entries[i]);
        if (Found f = above((uint32_t)n.entries[i], k)) return f;
    }
    return {};
}

// Largest key below k in the subtree.
Found T4Store::below(uint32_t at, Key k) {
    Node n = load(at);
    if (n.type == PACK) {
        for (auto it = n.pairs.rbegin(); it != n.pairs.rend(); ++it)
            if (it->first < k) return *it;
        return {};
    }
    int d = first_diff(k, n.pre);
    if (d < n.p) return byte_at(k, d) > byte_at(n.pre, d) ? last_in(at) : Found{};
    int b = byte_at(k, n.p);
    for (size_t j = n.bytes.size(); j-- > 0;) {
        if (n.bytes[j] > b) continue;
        if (is_values(n.type)) {
            if (n.bytes[j] < b) return Pair{n.pre | (Key)n.bytes[j], n.entries[j]};
            continue;
        }
        if (n.bytes[j] < b) return last_in((uint32_t)n.entries[j]);
        if (Found f = below((uint32_t)n.entries[j], k)) return f;
    }
    return {};
}

void T4Store::walk(uint32_t at, Pairs& out) {
    Node n = load(at);
    if (n.type == PACK) {
        out.insert(out.end(), n.pairs.begin(), n.pairs.end());
        return;
    }
    for (size_t i = 0; i < n.bytes.size(); ++i) {
        if (is_values(n.type)) out.push_back({n.pre | (Key)n.bytes[i], n.entries[i]});
        else walk((uint32_t)n.entries[i], out);
    }
}

Pairs T4Store::pairs(int s) {
    Pairs out;
    if (root_[s]) walk(root_[s], out);
    return out;
}

size_t T4Store::live_words(int s) {
    std::vector<uint32_t> todo;
    if (root_[s]) todo.push_back(root_[s]);
    size_t words = 0;
    while (!todo.empty()) {
        Node n = load(todo.back());
        todo.pop_back();
        if (n.type == PACK) words += pack_words((int)n.pairs.size(), n.flags);
        else if (is_values(n.type)) words += values_words((int)n.bytes.size(), n.pre);
        else {
            words += inner_words((int)n.bytes.size(), n.p, n.pre);
            for (uint64_t e : n.entries) todo.push_back((uint32_t)e);
        }
    }
    return words;
}

}  // namespace t4
