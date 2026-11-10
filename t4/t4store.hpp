// T4 store: every structure is a radix tree in one memory image of 8-byte words.
//
// SQ and set operations write a structure canonically: subtrees of up to 7 keys become packs,
// keys that differ only in the last byte become a value node, and the children of a node lie
// in one block right after each other. INS and DEL change the tree in place where the node
// does not grow; a grown node is written to free memory and its parent gets the new address.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "disc.hpp"
#include "node.hpp"

namespace t4 {

// A node read back from memory, branches in key order.
struct Node {
    uint32_t at = 0;
    Type type = PACK;
    uint64_t flags = 0;    // H and W of word 0
    int p = 0;             // byte the node branches on (15 for value nodes)
    Key pre = 0;           // bytes 0..p-1 shared by every key below
    std::vector<int> bytes;
    std::vector<uint64_t> entries;  // child addresses or values
    Pairs pairs;           // packs only
    uint32_t set_at = 0;   // word of the branch set
    uint32_t entry_at = 0;  // word of the first entry, or of the first pair of a pack
};

struct Stats {
    uint64_t node_reads = 0;    // nodes visited by all commands so far
    uint64_t words_written = 0;
};

class T4Store {
public:
    T4Store() : mem_(1, 0) {}  // word 0 stays unused, so address 0 means "no node"

    // Store interface of Disc.
    void clear(int s) { root_[s] = 0, size_[s] = 0; }
    void put(int s, Key k, uint64_t v);
    bool erase(int s, Key k);
    size_t size(int s) const { return size_[s]; }
    Found find(int s, Key k);
    Found above(int s, Key k) { return root_[s] ? above(root_[s], k) : Found{}; }
    Found below(int s, Key k) { return root_[s] ? below(root_[s], k) : Found{}; }
    Found first(int s) { return root_[s] ? first_in(root_[s]) : Found{}; }
    Found last(int s) { return root_[s] ? last_in(root_[s]) : Found{}; }
    Pairs pairs(int s);
    void assign(int s, const Pairs& sorted);
    void squeeze(int s) { assign(s, pairs(s)); }

    // Inspection for tests and reports.
    uint32_t root(int s) const { return root_[s]; }
    Node load(uint32_t at);
    size_t live_words(int s);
    const Stats& stats() const { return stats_; }
    const std::vector<uint64_t>& image() const { return mem_; }

private:
    uint32_t alloc(size_t words);
    uint32_t store(const std::vector<uint64_t>& w);
    void store_at(uint32_t at, const std::vector<uint64_t>& w);
    void set_entry(const Node& n, int i, uint64_t e);
    int find_branch(const Node& n, int b) const;

    uint32_t build(const Pairs& v);
    size_t node_words(const Pairs& v, size_t lo, size_t hi) const;
    void place(const Pairs& v, size_t lo, size_t hi, uint32_t at);

    uint32_t insert(uint32_t at, Key k, uint64_t v, bool& added);
    uint32_t remove(uint32_t at, Key k, bool& removed);
    Found above(uint32_t at, Key k);
    Found below(uint32_t at, Key k);
    Found first_in(uint32_t at);
    Found last_in(uint32_t at);
    void walk(uint32_t at, Pairs& out);

    std::vector<uint64_t> mem_;
    std::array<uint32_t, kStructs> root_{};
    std::array<size_t, kStructs> size_{};
    Stats stats_;
};

// Node encoding, shared with tests.
uint64_t pack_flags(const Pairs& p);
std::vector<uint64_t> encode_pack(const Pairs& p);
std::vector<uint64_t> encode_branches(bool values, int p, Key pre, const std::vector<int>& bytes,
                                      const std::vector<uint64_t>& entries);

}  // namespace t4

using T4Disc = Disc<t4::T4Store>;
