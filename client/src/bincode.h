#pragma once

// bincode 1.x (legacy config: little endian, fixed-width integers) writer/reader, and
// ALVR's hash_string (SipHash-1-3, zero key, message = string || 0xFF).
// Wire format reference: docs/alvr-20.14.1-protocol.md, sections 1 and 6.4.

#include <stdint.h>
#include <string.h>

uint64_t alvr_hash_string(const char *s);

struct BinWriter {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;

    void bytes(const void *p, size_t n)
    {
        if (len + n > cap) {
            overflow = true;
            return;
        }
        memcpy(buf + len, p, n);
        len += n;
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u32(uint32_t v) { bytes(&v, 4); } // x86: native order is little endian
    void u64(uint64_t v) { bytes(&v, 8); }
    void f32(float v) { bytes(&v, 4); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void variant(uint32_t index) { u32(index); }
    void str(const char *s)
    {
        size_t n = strlen(s);
        u64(n);
        bytes(s, n);
    }
    void duration_ns(uint64_t ns)
    {
        u64(ns / 1000000000ull);
        u32((uint32_t)(ns % 1000000000ull));
    }
};

struct BinReader {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    bool error;

    bool take(void *out, size_t n)
    {
        if (pos + n > len) {
            error = true;
            return false;
        }
        memcpy(out, buf + pos, n);
        pos += n;
        return true;
    }
    uint8_t u8()
    {
        uint8_t v = 0;
        take(&v, 1);
        return v;
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        take(&v, 4);
        return v;
    }
    uint64_t u64()
    {
        uint64_t v = 0;
        take(&v, 8);
        return v;
    }
    float f32()
    {
        float v = 0;
        take(&v, 4);
        return v;
    }
    uint64_t duration_ns()
    {
        uint64_t s = u64();
        uint32_t n = u32();
        return s * 1000000000ull + n;
    }
    // Length-prefixed bytes (String / Vec<u8>): returns a pointer into the buffer.
    const uint8_t *blob(uint64_t *out_len)
    {
        uint64_t n = u64();
        if (error || n > len - pos) { // pos <= len: no overflow with a huge n
            error = true;
            *out_len = 0;
            return nullptr;
        }
        const uint8_t *p = buf + pos;
        pos += n;
        *out_len = n;
        return p;
    }
};
