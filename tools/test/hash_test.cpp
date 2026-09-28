// Host check of alvr_hash_string against the ids listed in docs/alvr-20.14.1-protocol.md.
#include <stdio.h>
#include "bincode.h"
int main()
{
    struct { const char *s; uint64_t id; } cases[] = {
        {"20", 0x17667eafcf6d5d67ull},
        {"/user/head", 0x5b90853dc9202538ull},
        {"/user/hand/left", 0xe521d8dabe2d07a2ull},
        {"/user/hand/right", 0xf6b81330eb3dbdd8ull},
        {"/interaction_profiles/htc/vive_controller", 0x492bd9426d3d1ceaull},
        {"/user/hand/left/input/trigger/value", 0x0b66bbe3dc2dc49full},
    };
    int bad = 0;
    for (auto &c : cases) {
        uint64_t h = alvr_hash_string(c.s);
        printf("%-45s %016llx %s\n", c.s, (unsigned long long)h, h == c.id ? "ok" : "MISMATCH");
        bad += h != c.id;
    }
    return bad;
}
