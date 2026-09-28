#include "bincode.h"

static inline uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

#define SIPROUND                                                                                                       \
    do {                                                                                                               \
        v0 += v1;                                                                                                      \
        v1 = rotl64(v1, 13);                                                                                           \
        v1 ^= v0;                                                                                                      \
        v0 = rotl64(v0, 32);                                                                                           \
        v2 += v3;                                                                                                      \
        v3 = rotl64(v3, 16);                                                                                           \
        v3 ^= v2;                                                                                                      \
        v0 += v3;                                                                                                      \
        v3 = rotl64(v3, 21);                                                                                           \
        v3 ^= v0;                                                                                                      \
        v2 += v1;                                                                                                      \
        v1 = rotl64(v1, 17);                                                                                           \
        v1 ^= v2;                                                                                                      \
        v2 = rotl64(v2, 32);                                                                                           \
    } while (0)

// Rust's DefaultHasher (SipHash-1-3, keys 0/0) over `str::hash` = bytes then 0xFF.
uint64_t alvr_hash_string(const char *s)
{
    const size_t len = strlen(s), n = len + 1;
    auto msg = [&](size_t j) -> uint64_t { return j < len ? (uint64_t)(uint8_t)s[j] : 0xFFull; };
    uint64_t v0 = 0x736f6d6570736575ull, v1 = 0x646f72616e646f6dull;
    uint64_t v2 = 0x6c7967656e657261ull, v3 = 0x7465646279746573ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t m = 0;
        for (int k = 0; k < 8; k++)
            m |= msg(i + k) << (8 * k);
        v3 ^= m;
        SIPROUND;
        v0 ^= m;
    }
    uint64_t b = (uint64_t)(n & 0xff) << 56;
    for (size_t k = 0; i + k < n; k++)
        b |= msg(i + k) << (8 * k);
    v3 ^= b;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
