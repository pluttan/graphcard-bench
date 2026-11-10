// T4 node format in the memory image. Addresses and sizes count 8-byte words.
//
// A key has 16 bytes: the high half (bytes 0..7), then the low half (bytes 8..15).
//
// word 0 of every node: type (bits 0-3), flag H (bit 4), flag W (bit 5), byte position p or pack
//                       size n (bits 8-15), number of entries c (bits 16-24).
// PACK:   word 0, then n pairs, keys sorted. A subtree of up to 7 keys.
//         Every key has the same high half: zero, or with H the word after word 0, and the
//         pairs are (low half, value). With W the pairs are (high half, low half, value).
// INNER:  word 0, prefix, branch set, child addresses (4 bytes each).
//         Keys below share bytes 0..p-1 with the prefix and branch on byte p.
// VALUES: word 0, prefix, branch set, values (8 bytes each). Keys share bytes 0..14.
// Prefix: for p <= 8 one word, the high half of the shared bytes. For p > 8 the low half of
//         the shared bytes, after the high half when H is set; without H the high half is zero.
// Branch set: up to 16 sorted key bytes (LIST) or a 256-bit bitmap (BITMAP).
//
// Keys with a zero high half so take the words a 64-bit tree would take.
#pragma once
#include <cstdint>

#include "disc.hpp"

namespace t4 {

enum Type : uint8_t { PACK = 1, INNER_LIST = 2, INNER_BITMAP = 3, VALUES_LIST = 4, VALUES_BITMAP = 5 };

constexpr int kPackMax = 7;
constexpr int kListMax = 16;  // more branches switch the node to a bitmap
constexpr int kKeyBytes = 16;
constexpr int kLast = kKeyBytes - 1;  // byte position of value nodes
constexpr uint64_t kFlagH = 1u << 4;
constexpr uint64_t kFlagW = 1u << 5;

// Byte p of a key, p = 0 is the most significant byte.
inline int byte_at(Key k, int p) { return (int)((k >> (8 * (kLast - p))) & 0xff); }

// Bytes 0..p-1 of a key; the rest zero.
inline Key prefix(Key k, int p) { return p == 0 ? 0 : k & (~(Key)0 << (8 * (kKeyBytes - p))); }

inline int first_diff(Key a, Key b) {
    if (a == b) return kKeyBytes;
    uint64_t h = key_hi(a) ^ key_hi(b);
    return h ? __builtin_clzll(h) / 8 : 8 + __builtin_clzll(key_lo(a) ^ key_lo(b)) / 8;
}

inline uint64_t head(Type t, int pn, int c, uint64_t flags = 0) {
    return (uint64_t)t | flags | (uint64_t)pn << 8 | (uint64_t)c << 16;
}
inline Type type_of(uint64_t w) { return (Type)(w & 0xf); }
inline bool has_h(uint64_t w) { return w & kFlagH; }
inline bool has_w(uint64_t w) { return w & kFlagW; }
inline int pn_of(uint64_t w) { return (int)((w >> 8) & 0xff); }
inline int count_of(uint64_t w) { return (int)((w >> 16) & 0x1ff); }

inline bool is_inner(Type t) { return t == INNER_LIST || t == INNER_BITMAP; }
inline bool is_values(Type t) { return t == VALUES_LIST || t == VALUES_BITMAP; }
inline bool is_bitmap(Type t) { return t == INNER_BITMAP || t == VALUES_BITMAP; }

// Words of the branch set and of the entries for c branches.
inline int set_words(int c) { return c > kListMax ? 4 : (c + 7) / 8; }
inline int addr_words(int c) { return (c + 1) / 2; }

// A branch node keeps the high half of its prefix in a word of its own only past byte 8.
inline bool pre_wide(int p, Key pre) { return p > 8 && key_hi(pre) != 0; }
inline int pre_words(int p, Key pre) { return pre_wide(p, pre) ? 2 : 1; }

inline int pack_words(int n, uint64_t flags) {
    return 1 + (flags & kFlagH ? 1 : 0) + n * (flags & kFlagW ? 3 : 2);
}
inline int inner_words(int c, int p, Key pre) { return 1 + pre_words(p, pre) + set_words(c) + addr_words(c); }
inline int values_words(int c, Key pre) { return 1 + pre_words(kLast, pre) + set_words(c) + c; }

// Rank of byte b in a 256-bit bitmap: the number of set bits below it.
inline int rank(const uint64_t* bm, int b) {
    int r = 0;
    for (int w = 0; w < b / 64; ++w) r += __builtin_popcountll(bm[w]);
    uint64_t low = (b % 64) ? bm[b / 64] & ((1ull << (b % 64)) - 1) : 0;
    return r + __builtin_popcountll(low);
}

inline bool has_bit(const uint64_t* bm, int b) { return (bm[b / 64] >> (b % 64)) & 1; }

}  // namespace t4
