#include "mtfs_sentinel_sha256.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include <string.h>

typedef struct sha256_context
{
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
    uint32_t used;
} sha256_context_t;

static uint32_t rotate_right(uint32_t value, uint32_t amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static uint32_t big32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
        ((uint32_t)bytes[2] << 8U) | bytes[3];
}

static void put_big32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void transform(sha256_context_t *context, const uint8_t block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,
        0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,
        0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,
        0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,
        0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,
        0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,
        0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,
        0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
        0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };
    uint32_t words[64], a, b, c, d, e, f, g, h, i;
    for (i = 0U; i < 16U; ++i) words[i] = big32(block + i * 4U);
    for (i = 16U; i < 64U; ++i) {
        uint32_t s0 = rotate_right(words[i - 15U], 7U) ^
            rotate_right(words[i - 15U], 18U) ^ (words[i - 15U] >> 3U);
        uint32_t s1 = rotate_right(words[i - 2U], 17U) ^
            rotate_right(words[i - 2U], 19U) ^ (words[i - 2U] >> 10U);
        words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }
    a=context->state[0]; b=context->state[1]; c=context->state[2]; d=context->state[3];
    e=context->state[4]; f=context->state[5]; g=context->state[6]; h=context->state[7];
    for (i = 0U; i < 64U; ++i) {
        uint32_t s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choice + constants[i] + words[i];
        uint32_t s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    context->state[0]+=a; context->state[1]+=b; context->state[2]+=c;
    context->state[3]+=d; context->state[4]+=e; context->state[5]+=f;
    context->state[6]+=g; context->state[7]+=h;
    (void)memset(words, 0, sizeof(words));
}

static void update(sha256_context_t *context, const uint8_t *bytes, size_t size)
{
    context->bytes += size;
    while (size != 0U) {
        size_t available = sizeof(context->block) - context->used;
        size_t take = size < available ? size : available;
        (void)memcpy(context->block + context->used, bytes, take);
        context->used += (uint32_t)take; bytes += take; size -= take;
        if (context->used == sizeof(context->block)) {
            transform(context, context->block); context->used = 0U;
        }
    }
}

mtfs_error_t mtfs_sentinel_sha256(const void *input, size_t size,
    uint8_t digest[32])
{
    static const uint32_t initial[8] = {0x6a09e667U,0xbb67ae85U,
        0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,
        0x1f83d9abU,0x5be0cd19U};
    sha256_context_t context;
    uint64_t bits;
    uint32_t i;
    if ((input == NULL && size != 0U) || digest == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
#if SIZE_MAX > (UINT64_MAX / 8U)
    if (size > (size_t)(UINT64_MAX / 8U))
        return MTFS_ERROR_INVALID_ARGUMENT;
#endif
    (void)memset(&context, 0, sizeof(context));
    (void)memcpy(context.state, initial, sizeof(initial));
    update(&context, (const uint8_t *)input, size);
    bits = context.bytes * 8U;
    context.block[context.used++] = 0x80U;
    if (context.used > 56U) {
        (void)memset(context.block + context.used, 0, 64U - context.used);
        transform(&context, context.block); context.used = 0U;
    }
    (void)memset(context.block + context.used, 0, 56U - context.used);
    for (i = 0U; i < 8U; ++i)
        context.block[63U - i] = (uint8_t)(bits >> (i * 8U));
    transform(&context, context.block);
    for (i = 0U; i < 8U; ++i) put_big32(digest + i * 4U, context.state[i]);
    (void)memset(&context, 0, sizeof(context));
    return MTFS_OK;
}

#else
typedef int mtfs_sentinel_sha256_disabled_translation_unit_t;
#endif
