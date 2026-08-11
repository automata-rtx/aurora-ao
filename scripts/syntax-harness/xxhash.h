// Minimal xxHash shim for the syntax harness - declarations only, no algorithm.
#pragma once
#include <cstddef>
#include <cstdint>
typedef uint32_t XXH32_hash_t;
typedef uint64_t XXH64_hash_t;
typedef struct { XXH64_hash_t low64; XXH64_hash_t high64; } XXH128_hash_t;
typedef struct { unsigned char opaque[576]; } XXH3_state_t;
#define XXH3_INITSTATE(p) ((void)(p))
extern "C" {
XXH64_hash_t XXH64(const void*, size_t, XXH64_hash_t);
XXH128_hash_t XXH128(const void*, size_t, XXH64_hash_t);
XXH64_hash_t XXH3_64bits_withSeed(const void*, size_t, XXH64_hash_t);
int XXH3_64bits_reset_withSeed(XXH3_state_t*, XXH64_hash_t);
int XXH3_64bits_update(XXH3_state_t*, const void*, size_t);
XXH64_hash_t XXH3_64bits_digest(const XXH3_state_t*);
}
