#include <openssl/evp.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtfs_sealed_blob.h"
#include "mtfs_model_store.h"
#include "../common/mtfs_model_test_registry.h"
#include "mtfs_secure_zero.h"

typedef struct memory_reader
{
    const uint8_t *data;
    size_t size;
    unsigned get_size_calls;
    unsigned read_calls;
    unsigned fail_read_call;
    unsigned short_read_call;
    mtfs_error_t size_error;
    mtfs_error_t read_error;
} memory_reader_t;

typedef struct test_provider
{
    uint8_t fleet_key[32];
    uint8_t model_key[32];
    uint8_t private_scratch[MTFS_SEALED_MAX_CHUNK_SIZE];
    unsigned open_fleet_calls;
    unsigned open_model_calls;
    unsigned decrypt_calls;
    unsigned close_calls;
    unsigned fail_call;
    int return_invalid_fleet_handle;
    int fleet_open;
    int model_open;
} test_provider_t;

typedef struct test_env
{
    mtfs_sealed_blob_t blob;
    mtfs_model_t model;
    memory_reader_t memory;
    test_provider_t crypto;
    mtfs_sealed_reader_t reader;
    mtfs_crypto_provider_t provider;
    mtfs_sealed_work_t work;
    uint8_t manifest[MTFS_SEALED_MAX_MANIFEST_SIZE];
    uint8_t aad[MTFS_SEALED_CHUNK_AAD_SIZE];
    uint8_t ciphertext[MTFS_SEALED_CIPHER_BUFFER_SIZE];
    uint8_t plaintext[MTFS_SEALED_MAX_CHUNK_SIZE];
} test_env_t;

static unsigned checks;
static unsigned tests;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return 0; } } while (0)

static void model_policy_init(mtfs_model_policy_t *policy);

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
        return NULL;
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)length + 1U);
    if (data == NULL || fread(data, 1U, (size_t)length, file) != (size_t)length)
    {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static mtfs_error_t memory_get_size(void *context, uint64_t *size)
{
    memory_reader_t *reader = (memory_reader_t *)context;
    ++reader->get_size_calls;
    if (reader->size_error != MTFS_OK)
        return reader->size_error;
    *size = reader->size;
    return MTFS_OK;
}

static mtfs_error_t memory_read_at(void *context, uint64_t offset, void *buffer,
                                   size_t requested, size_t *read_size)
{
    memory_reader_t *reader = (memory_reader_t *)context;
    ++reader->read_calls;
    *read_size = 0U;
    if (reader->fail_read_call == reader->read_calls)
        return reader->read_error != MTFS_OK ? reader->read_error : MTFS_ERROR_IO;
    if (offset > reader->size || requested > reader->size - (size_t)offset)
        return MTFS_ERROR_IO;
    if (reader->short_read_call == reader->read_calls && requested != 0U)
        --requested;
    memcpy(buffer, reader->data + (size_t)offset, requested);
    *read_size = requested;
    return MTFS_OK;
}

static int aes_gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t *ciphertext,
    size_t ciphertext_size, const uint8_t tag[16], uint8_t *plaintext)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int output = 0;
    int final_size = 0;
    int ok = ctx != NULL && aad_size <= INT_MAX && ciphertext_size <= INT_MAX &&
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
        EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
        EVP_DecryptUpdate(ctx, NULL, &output, aad, (int)aad_size) == 1 &&
        EVP_DecryptUpdate(ctx, plaintext, &output, ciphertext,
                          (int)ciphertext_size) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) == 1 &&
        EVP_DecryptFinal_ex(ctx, plaintext + output, &final_size) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static int aes_gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t *plaintext,
    size_t plaintext_size, uint8_t *ciphertext, uint8_t tag[16])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int output = 0;
    int final_size = 0;
    int ok = ctx != NULL && aad_size <= INT_MAX && plaintext_size <= INT_MAX &&
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
        EVP_EncryptUpdate(ctx, NULL, &output, aad, (int)aad_size) == 1 &&
        EVP_EncryptUpdate(ctx, ciphertext, &output, plaintext,
                          (int)plaintext_size) == 1 &&
        EVP_EncryptFinal_ex(ctx, ciphertext + output, &final_size) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static void test_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void test_put_u64(uint8_t *p, uint64_t value)
{
    test_put_u32(p, (uint32_t)value);
    test_put_u32(p + 4U, (uint32_t)(value >> 32));
}

static int make_required_ram_package(const uint8_t *package, size_t package_size,
    const uint8_t fleet_key[32], uint64_t required_ram, uint8_t *output)
{
    static const uint8_t key_domain[12] =
        {'M','T','F','S','-','K','E','Y','-','v','1',0};
    static const uint8_t chunk_domain[14] =
        {'M','T','F','S','-','C','H','U','N','K','-','v','1',0};
    uint8_t model_key[32];
    uint8_t plain[4096];
    uint8_t old_aad[214];
    uint8_t new_aad[214];
    uint8_t nonce[12];
    size_t offset = 240U;
    uint32_t index;
    if (package_size != 5272U)
        return 0;
    memcpy(output, package, package_size);
    memcpy(old_aad, key_domain, sizeof(key_domain));
    memcpy(old_aad + sizeof(key_domain), package, 192U);
    if (!aes_gcm_decrypt(fleet_key, package + 128U, old_aad, 204U,
                         package + 192U, 32U, package + 224U, model_key))
        return 0;
    test_put_u64(output + 96U, required_ram);
    memcpy(new_aad, key_domain, sizeof(key_domain));
    memcpy(new_aad + sizeof(key_domain), output, 192U);
    if (!aes_gcm_encrypt(fleet_key, output + 128U, new_aad, 204U, model_key,
                         sizeof(model_key), output + 192U, output + 224U))
        return 0;
    for (index = 0U; index < 2U; ++index)
    {
        size_t length = index == 0U ? 4096U : 904U;
        memcpy(nonce, package + 140U, 8U);
        test_put_u32(nonce + 8U, index);
        memcpy(old_aad, chunk_domain, sizeof(chunk_domain));
        memcpy(old_aad + sizeof(chunk_domain), package, 192U);
        test_put_u32(old_aad + 206U, index);
        test_put_u32(old_aad + 210U, (uint32_t)length);
        if (!aes_gcm_decrypt(model_key, nonce, old_aad, sizeof(old_aad),
                             package + offset, length, package + offset + length,
                             plain))
            return 0;
        memcpy(new_aad, chunk_domain, sizeof(chunk_domain));
        memcpy(new_aad + sizeof(chunk_domain), output, 192U);
        test_put_u32(new_aad + 206U, index);
        test_put_u32(new_aad + 210U, (uint32_t)length);
        if (!aes_gcm_encrypt(model_key, nonce, new_aad, sizeof(new_aad), plain,
                             length, output + offset, output + offset + length))
            return 0;
        offset += length + MTFS_SEALED_TAG_SIZE;
    }
    mtfs_secure_zero(model_key, sizeof(model_key));
    mtfs_secure_zero(plain, sizeof(plain));
    return 1;
}

static mtfs_crypto_status_t provider_open_fleet(void *context, uint32_t key_id,
    uint32_t key_version, mtfs_crypto_key_handle_t *handle)
{
    test_provider_t *provider = (test_provider_t *)context;
    ++provider->open_fleet_calls;
    *handle = 0U;
    if (provider->fail_call == provider->open_fleet_calls)
        return MTFS_CRYPTO_FAILED;
    if (key_id != 1U || key_version != 1U)
        return MTFS_CRYPTO_NOT_SUPPORTED;
    if (provider->return_invalid_fleet_handle)
        return MTFS_CRYPTO_OK;
    provider->fleet_open = 1;
    *handle = 1U;
    return MTFS_CRYPTO_OK;
}

static mtfs_crypto_status_t provider_open_model(void *context,
    mtfs_crypto_key_handle_t fleet_handle, const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t ciphertext[32],
    const uint8_t tag[16], mtfs_crypto_key_handle_t *handle)
{
    test_provider_t *provider = (test_provider_t *)context;
    uint8_t candidate[32];
    ++provider->open_model_calls;
    *handle = 0U;
    if (provider->fail_call == provider->open_fleet_calls + provider->open_model_calls)
        return MTFS_CRYPTO_FAILED;
    if (fleet_handle != 1U || !provider->fleet_open)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    if (!aes_gcm_decrypt(provider->fleet_key, nonce, aad, aad_size,
                         ciphertext, 32U, tag, candidate))
    {
        mtfs_secure_zero(candidate, sizeof(candidate));
        return MTFS_CRYPTO_AUTHENTICATION_FAILED;
    }
    memcpy(provider->model_key, candidate, sizeof(candidate));
    mtfs_secure_zero(candidate, sizeof(candidate));
    provider->model_open = 1;
    *handle = 2U;
    return MTFS_CRYPTO_OK;
}

static mtfs_crypto_status_t provider_decrypt(void *context,
    mtfs_crypto_key_handle_t model_handle, const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t *ciphertext,
    size_t ciphertext_size, const uint8_t tag[16], uint8_t *plaintext)
{
    test_provider_t *provider = (test_provider_t *)context;
    ++provider->decrypt_calls;
    if (provider->fail_call == provider->open_fleet_calls +
        provider->open_model_calls + provider->decrypt_calls)
        return MTFS_CRYPTO_FAILED;
    if (model_handle != 2U || !provider->model_open ||
        ciphertext_size > sizeof(provider->private_scratch))
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    if (!aes_gcm_decrypt(provider->model_key, nonce, aad, aad_size, ciphertext,
                         ciphertext_size, tag, provider->private_scratch))
    {
        mtfs_secure_zero(provider->private_scratch,
                         sizeof(provider->private_scratch));
        return MTFS_CRYPTO_AUTHENTICATION_FAILED;
    }
    memcpy(plaintext, provider->private_scratch, ciphertext_size);
    mtfs_secure_zero(provider->private_scratch, sizeof(provider->private_scratch));
    return MTFS_CRYPTO_OK;
}

static mtfs_crypto_status_t provider_close(void *context,
                                           mtfs_crypto_key_handle_t handle)
{
    test_provider_t *provider = (test_provider_t *)context;
    if (handle == 2U && provider->model_open)
    {
        provider->model_open = 0;
        mtfs_secure_zero(provider->model_key, sizeof(provider->model_key));
    }
    else if (handle == 1U && provider->fleet_open)
        provider->fleet_open = 0;
    else
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    ++provider->close_calls;
    return MTFS_CRYPTO_OK;
}

static void env_init(test_env_t *env, const uint8_t *package, size_t package_size,
                     const uint8_t fleet_key[32])
{
    memset(env, 0, sizeof(*env));
    mtfs_sealed_blob_init(&env->blob);
    mtfs_model_init(&env->model);
    env->memory.data = package;
    env->memory.size = package_size;
    env->reader.api_version = MTFS_SEALED_READER_API_VERSION;
    env->reader.struct_size = (uint32_t)sizeof(env->reader);
    env->reader.context = &env->memory;
    env->reader.get_size = memory_get_size;
    env->reader.read_at = memory_read_at;
    memcpy(env->crypto.fleet_key, fleet_key, 32U);
    env->provider.api_version = MTFS_CRYPTO_PROVIDER_API_VERSION;
    env->provider.struct_size = (uint32_t)sizeof(env->provider);
    env->provider.context = &env->crypto;
    env->provider.open_fleet_key = provider_open_fleet;
    env->provider.open_model_key = provider_open_model;
    env->provider.decrypt_chunk = provider_decrypt;
    env->provider.close_key = provider_close;
    env->work.api_version = MTFS_SEALED_WORK_API_VERSION;
    env->work.struct_size = (uint32_t)sizeof(env->work);
    env->work.manifest = env->manifest;
    env->work.manifest_capacity = sizeof(env->manifest);
    env->work.aad = env->aad;
    env->work.aad_capacity = sizeof(env->aad);
    env->work.ciphertext = env->ciphertext;
    env->work.ciphertext_capacity = sizeof(env->ciphertext);
    env->work.plaintext = env->plaintext;
    env->work.plaintext_capacity = sizeof(env->plaintext);
}

static int all_value(const uint8_t *data, size_t size, uint8_t value)
{
    size_t i;
    for (i = 0U; i < size; ++i)
        if (data[i] != value)
            return 0;
    return 1;
}

static int test_golden(const uint8_t *package, size_t package_size,
                       const uint8_t *payload, size_t payload_size,
                       const uint8_t fleet_key[32])
{
    test_env_t env;
    mtfs_sealed_package_info_t info;
    uint8_t destination[6000];
    size_t loaded = 123U;
    env_init(&env, package, package_size, fleet_key);
    memset(destination, 0xA5, sizeof(destination));
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_OK);
    CHECK(info.payload_plain_length == payload_size);
    CHECK(info.chunk_plain_size == 4096U && info.chunk_count == 2U);
    CHECK(env.blob.state == MTFS_SEALED_BLOB_OPEN);
    CHECK(mtfs_sealed_blob_load(&env.blob, destination, sizeof(destination),
                                &loaded) == MTFS_OK);
    CHECK(loaded == payload_size && memcmp(destination, payload, payload_size) == 0);
    CHECK(all_value(destination + payload_size,
                    sizeof(destination) - payload_size, 0xA5U));
    CHECK(env.crypto.decrypt_calls == 2U && env.blob.state == MTFS_SEALED_BLOB_LOADED);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    CHECK(env.crypto.close_calls == 2U && !env.crypto.fleet_open && !env.crypto.model_open);
    CHECK(all_value(env.manifest, sizeof(env.manifest), 0U));
    CHECK(all_value(env.aad, sizeof(env.aad), 0U));
    CHECK(all_value(env.ciphertext, sizeof(env.ciphertext), 0U));
    CHECK(all_value(env.plaintext, sizeof(env.plaintext), 0U));
    CHECK(memcmp(destination, payload, payload_size) == 0);
    return 1;
}

static int test_mutations(const uint8_t *package, size_t package_size,
                          const uint8_t fleet_key[32])
{
    static const struct mutation { size_t offset; size_t zero_size; int payload; } cases[] = {
        {0,0,0},{8,0,0},{10,0,0},{12,0,0},{16,0,0},{20,0,0},{24,0,0},
        {28,0,0},{30,0,0},{32,0,0},{36,0,0},{40,16,0},{92,0,0},
        {104,8,0},{112,4,0},{116,0,0},{120,0,0},{124,0,0},{128,12,0},
        {140,8,0},{148,0,0},{160,0,0},{162,0,0},{164,4,0},{174,0,0},
        {192,0,0},{239,0,0},{240,0,1},{4336,0,1},{5271,0,1}
    };
    size_t i;
    CHECK(package_size == 5272U);
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        uint8_t copy[5272];
        test_env_t env;
        mtfs_sealed_package_info_t info;
        mtfs_error_t result;
        memcpy(copy, package, package_size);
        if (cases[i].zero_size != 0U)
        {
            if (cases[i].offset == 104U)
                memset(copy + cases[i].offset, 0xFF, cases[i].zero_size);
            else
                memset(copy + cases[i].offset, 0, cases[i].zero_size);
        }
        else
            copy[cases[i].offset] ^= 1U;
        env_init(&env, copy, package_size, fleet_key);
        memset(&info, 0x5A, sizeof(info));
        result = mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                       &env.work, &info);
        if (cases[i].payload)
        {
            uint8_t destination[5017];
            size_t loaded = 9U;
            CHECK(result == MTFS_OK);
            memset(destination, 0xA5, sizeof(destination));
            CHECK(mtfs_sealed_blob_load(&env.blob, destination, sizeof(destination),
                                        &loaded) != MTFS_OK);
            CHECK(loaded == 0U && all_value(destination, 5000U, 0U));
            CHECK(all_value(destination + 5000U, 17U, 0xA5U));
            CHECK(env.blob.state == MTFS_SEALED_BLOB_ERROR && !env.crypto.model_open);
        }
        else
        {
            CHECK(result != MTFS_OK);
            CHECK(all_value((const uint8_t *)&info, sizeof(info), 0x5AU));
        }
        CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
        CHECK(!env.crypto.fleet_open && !env.crypto.model_open);
    }
    return 1;
}

static int test_layout_failures(const uint8_t *package, size_t package_size,
                                const uint8_t fleet_key[32])
{
    static const size_t truncations[] =
        {0U,7U,8U,12U,159U,160U,191U,192U,239U,240U,4335U,4336U,4351U,4352U,5271U};
    size_t i;
    for (i = 0U; i < sizeof(truncations) / sizeof(truncations[0]); ++i)
    {
        test_env_t env;
        mtfs_sealed_package_info_t info;
        env_init(&env, package, truncations[i], fleet_key);
        CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                    &env.work, &info) != MTFS_OK);
        CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    }
    {
        uint8_t trailing[5273];
        test_env_t env;
        mtfs_sealed_package_info_t info;
        memcpy(trailing, package, package_size);
        trailing[package_size] = 0U;
        env_init(&env, trailing, package_size + 1U, fleet_key);
        CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                    &env.work, &info) == MTFS_ERROR_MALFORMED_FORMAT);
        CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    }
    return 1;
}

static int test_faults_and_contracts(const uint8_t *package, size_t package_size,
                                     const uint8_t fleet_key[32])
{
    test_env_t env;
    mtfs_sealed_package_info_t info;
    uint8_t destination[5017];
    size_t loaded;
    env_init(&env, package, package_size, fleet_key);
    env.work.manifest_capacity--;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_BUFFER_TOO_SMALL);
    CHECK(env.memory.get_size_calls == 0U && env.memory.read_calls == 0U);
    env_init(&env, package, package_size, fleet_key);
    env.work.aad_capacity--;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_BUFFER_TOO_SMALL);
    CHECK(env.memory.get_size_calls == 0U);
    env_init(&env, package, package_size, fleet_key);
    memset(&info, 0x5A, sizeof(info));
    memset(env.manifest, 0xA5, sizeof(env.manifest));
    memset(env.aad, 0xA5, sizeof(env.aad));
    memset(env.ciphertext, 0xA5, sizeof(env.ciphertext));
    memset(env.plaintext, 0xA5, sizeof(env.plaintext));
    env.crypto.return_invalid_fleet_handle = 1;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_CRYPTO);
    CHECK(env.blob.state == MTFS_SEALED_BLOB_ERROR);
    CHECK(env.blob.fleet_handle == MTFS_CRYPTO_INVALID_KEY_HANDLE &&
          env.blob.model_handle == MTFS_CRYPTO_INVALID_KEY_HANDLE);
    CHECK(env.crypto.close_calls == 0U && !env.crypto.fleet_open &&
          !env.crypto.model_open);
    CHECK(all_value((const uint8_t *)&info, sizeof(info), 0x5AU));
    CHECK(all_value(env.manifest, sizeof(env.manifest), 0U));
    CHECK(all_value(env.aad, sizeof(env.aad), 0U));
    CHECK(all_value(env.ciphertext, sizeof(env.ciphertext), 0U));
    CHECK(all_value(env.plaintext, sizeof(env.plaintext), 0U));
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    env_init(&env, package, package_size, fleet_key);
    env.work.ciphertext_capacity--;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_BUFFER_TOO_SMALL);
    CHECK(env.memory.get_size_calls == 0U);
    env_init(&env, package, package_size, fleet_key);
    env.work.plaintext_capacity--;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_BUFFER_TOO_SMALL);
    CHECK(env.memory.get_size_calls == 0U);
    env_init(&env, package, package_size, fleet_key);
    env.work.aad = env.manifest;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_INVALID_ARGUMENT);
    CHECK(env.memory.get_size_calls == 0U);
    env_init(&env, package, package_size, fleet_key);
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work,
                                (mtfs_sealed_package_info_t *)env.manifest) ==
          MTFS_ERROR_INVALID_ARGUMENT);
    CHECK(env.memory.get_size_calls == 0U);
    env_init(&env, package, package_size, fleet_key);
    env.memory.size_error = MTFS_ERROR_IO;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_IO);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    env_init(&env, package, package_size, fleet_key);
    env.memory.short_read_call = 1U;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_IO);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    env_init(&env, package, package_size, fleet_key);
    env.crypto.fail_call = 2U;
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_CRYPTO);
    CHECK(env.crypto.close_calls == 1U && !env.crypto.fleet_open);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    env_init(&env, package, package_size, fleet_key);
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_OK);
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_INVALID_STATE);
    memset(destination, 0xA5, sizeof(destination));
    loaded = 8U;
    CHECK(mtfs_sealed_blob_load(&env.blob, destination, 4999U, &loaded) ==
          MTFS_ERROR_BUFFER_TOO_SMALL);
    CHECK(loaded == 0U && all_value(destination, sizeof(destination), 0xA5U));
    CHECK(env.memory.read_calls == 3U);
    env.crypto.fail_call = 3U;
    CHECK(mtfs_sealed_blob_load(&env.blob, destination, sizeof(destination),
                                &loaded) == MTFS_ERROR_CRYPTO);
    CHECK(all_value(destination, 5000U, 0U));
    CHECK(all_value(destination + 5000U, 17U, 0xA5U));
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    env_init(&env, package, package_size, fleet_key);
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_OK);
    memset(destination, 0xA5, sizeof(destination));
    loaded = 7U;
    env.memory.fail_read_call = env.memory.read_calls + 1U;
    CHECK(mtfs_sealed_blob_load(&env.blob, destination, sizeof(destination),
                                &loaded) == MTFS_ERROR_IO);
    CHECK(loaded == 0U && all_value(destination, 5000U, 0U));
    CHECK(all_value(destination + 5000U, 17U, 0xA5U));
    CHECK(env.crypto.close_calls == 2U);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    env_init(&env, package, package_size, fleet_key);
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_OK);
    loaded = 7U;
    CHECK(mtfs_sealed_blob_load(&env.blob, env.plaintext, sizeof(env.plaintext),
                                &loaded) == MTFS_ERROR_INVALID_ARGUMENT);
    CHECK(loaded == 0U && env.blob.state == MTFS_SEALED_BLOB_OPEN);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    env_init(&env, package, package_size, fleet_key);
    loaded = 1U;
    CHECK(mtfs_sealed_blob_load(&env.blob, destination, sizeof(destination),
                                &loaded) == MTFS_ERROR_INVALID_STATE);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    return 1;
}

static int test_empty_payload(const uint8_t *package, size_t package_size,
                              const uint8_t fleet_key[32])
{
    static const uint8_t domain[12] =
        {'M','T','F','S','-','K','E','Y','-','v','1',0};
    uint8_t empty[240];
    uint8_t original_aad[204];
    uint8_t empty_aad[204];
    uint8_t model_key[32];
    test_env_t env;
    mtfs_sealed_package_info_t info;
    size_t loaded = 99U;
    CHECK(package_size >= sizeof(empty));
    memcpy(original_aad, domain, sizeof(domain));
    memcpy(original_aad + sizeof(domain), package, 192U);
    CHECK(aes_gcm_decrypt(fleet_key, package + 128U, original_aad,
                          sizeof(original_aad), package + 192U, 32U,
                          package + 224U, model_key));
    memcpy(empty, package, 192U);
    memset(empty + 96U, 0, 8U);
    memset(empty + 104U, 0, 8U);
    memset(empty + 116U, 0, 4U);
    memcpy(empty_aad, domain, sizeof(domain));
    memcpy(empty_aad + sizeof(domain), empty, 192U);
    CHECK(aes_gcm_encrypt(fleet_key, empty + 128U, empty_aad, sizeof(empty_aad),
                          model_key, sizeof(model_key), empty + 192U, empty + 224U));
    mtfs_secure_zero(model_key, sizeof(model_key));
    env_init(&env, empty, sizeof(empty), fleet_key);
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_OK);
    CHECK(info.payload_plain_length == 0U && info.chunk_count == 0U &&
          info.required_ram == 0U);
    CHECK(mtfs_sealed_blob_load(&env.blob, NULL, 0U, &loaded) == MTFS_OK);
    CHECK(loaded == 0U && env.crypto.decrypt_calls == 0U &&
          env.blob.state == MTFS_SEALED_BLOB_LOADED);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    CHECK(env.crypto.close_calls == 2U);
    {
        mtfs_model_policy_t policy;
        mtfs_model_info_t model_info;
        env_init(&env, empty, sizeof(empty), fleet_key);
        model_policy_init(&policy);
        CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider,
                              &env.work, &policy) == MTFS_OK);
        CHECK(mtfs_model_get_info(&env.model, &model_info) == MTFS_OK);
        CHECK(model_info.payload_size == 0U && model_info.required_ram == 0U &&
              model_info.chunk_count == 0U);
        loaded = 99U;
        CHECK(mtfs_model_load(&env.model, NULL, 0U, &loaded) == MTFS_OK);
        CHECK(loaded == 0U && env.crypto.decrypt_calls == 0U);
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    }
    return 1;
}

static int test_wrong_key(const uint8_t *package, size_t package_size,
                          const uint8_t fleet_key[32])
{
    uint8_t wrong[32];
    test_env_t env;
    mtfs_sealed_package_info_t info;
    memcpy(wrong, fleet_key, sizeof(wrong));
    wrong[0] ^= 1U;
    env_init(&env, package, package_size, wrong);
    CHECK(mtfs_sealed_blob_open(&env.blob, &env.reader, &env.provider,
                                &env.work, &info) == MTFS_ERROR_AUTHENTICATION);
    CHECK(env.crypto.close_calls == 1U && !env.crypto.fleet_open && !env.crypto.model_open);
    CHECK(mtfs_sealed_blob_close(&env.blob) == MTFS_OK);
    return 1;
}

static int test_metadata_rules(const uint8_t *package, size_t package_size,
                               const uint8_t fleet_key[32])
{
    uint8_t metadata[32] = {
        1,0,1,0, 4,0,0,0, 1,2,3,4, 0,0,0,0,
        4,0,0,0, 1,0,0,0, 'a',0,0,0, 0,0,0,0};
    uint8_t copy[32];
    (void)package;
    (void)package_size;
    (void)fleet_key;
    CHECK(mtfs_sealed_metadata_validate(metadata, sizeof(metadata)) == MTFS_OK);
    memcpy(copy, metadata, sizeof(copy));
    copy[16] = 5U;
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) == MTFS_OK);
    copy[18] = 1U;
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) ==
          MTFS_ERROR_UNSUPPORTED_FORMAT);
    memcpy(copy, metadata, sizeof(copy));
    copy[16] = 1U;
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) ==
          MTFS_ERROR_MALFORMED_FORMAT);
    memcpy(copy, metadata, sizeof(copy));
    copy[2] = 2U;
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) ==
          MTFS_ERROR_MALFORMED_FORMAT);
    memcpy(copy, metadata, sizeof(copy));
    memset(copy + 4U, 0xFF, 4U);
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) ==
          MTFS_ERROR_MALFORMED_FORMAT);
    memcpy(copy, metadata, sizeof(copy));
    copy[12] = 1U;
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) ==
          MTFS_ERROR_MALFORMED_FORMAT);
    memcpy(copy, metadata, sizeof(copy));
    copy[24] = 0U;
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) ==
          MTFS_ERROR_MALFORMED_FORMAT);
    memcpy(copy, metadata, sizeof(copy));
    copy[20] = 2U;
    copy[24] = 0xC0U;
    copy[25] = 0x80U;
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) ==
          MTFS_ERROR_MALFORMED_FORMAT);
    CHECK(mtfs_sealed_metadata_validate(metadata, sizeof(metadata) - 1U) ==
          MTFS_ERROR_MALFORMED_FORMAT);
    CHECK(mtfs_sealed_metadata_validate(metadata,
          MTFS_SEALED_MAX_METADATA_SIZE + 1U) == MTFS_ERROR_MALFORMED_FORMAT);
    memcpy(copy, metadata, sizeof(copy));
    copy[0] = 0U;
    CHECK(mtfs_sealed_metadata_validate(copy, sizeof(copy)) ==
          MTFS_ERROR_MALFORMED_FORMAT);
    return 1;
}

static int test_layout_chunk_policy(const uint8_t *package, size_t package_size,
                                    const uint8_t fleet_key[32])
{
    static const uint32_t invalid_sizes[] = {0U, 4095U, 65537U, 6000U};
    static const uint32_t valid_sizes[] = {4096U, 16384U, 65536U};
    mtfs_sealed_package_info_t info;
    mtfs_sealed_layout_t layout;
    size_t i;
    (void)package;
    (void)package_size;
    (void)fleet_key;
    memset(&info, 0, sizeof(info));
    memset(&layout, 0, sizeof(layout));
    info.payload_plain_length = 1U;
    info.chunk_count = 1U;
    layout.payload_offset = 208U;
    for (i = 0U; i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++i)
    {
        info.chunk_plain_size = invalid_sizes[i];
        CHECK(mtfs_sealed_format_finish_layout(&info, &layout) ==
              MTFS_ERROR_MALFORMED_FORMAT);
    }
    for (i = 0U; i < sizeof(valid_sizes) / sizeof(valid_sizes[0]); ++i)
    {
        info.chunk_plain_size = valid_sizes[i];
        CHECK(mtfs_sealed_format_finish_layout(&info, &layout) == MTFS_OK);
        CHECK(layout.expected_file_size == 225U);
    }
    info.payload_plain_length = 0U;
    info.chunk_count = 0U;
    info.chunk_plain_size = 4096U;
    CHECK(mtfs_sealed_format_finish_layout(&info, &layout) == MTFS_OK);
    CHECK(layout.expected_file_size == layout.payload_offset);
    return 1;
}

static void model_policy_init(mtfs_model_policy_t *policy)
{
    memset(policy, 0, sizeof(*policy));
    policy->api_version = MTFS_MODEL_POLICY_API_VERSION;
    policy->struct_size = (uint32_t)sizeof(*policy);
    policy->expected_target_id = MTFS_MODEL_TEST_TARGET_ID;
    policy->expected_accelerator_id = MTFS_MODEL_TEST_ACCELERATOR_ID;
    policy->accepted_model_format = MTFS_MODEL_TEST_FORMAT_ID;
    policy->maximum_chunk_size = MTFS_SEALED_MAX_CHUNK_SIZE;
    policy->maximum_model_payload_size = UINT64_C(5000);
    policy->maximum_required_ram = UINT64_C(5000);
}

static int test_model_golden(const uint8_t *package, size_t package_size,
                             const uint8_t *payload, size_t payload_size,
                             const uint8_t fleet_key[32])
{
    test_env_t env;
    mtfs_model_policy_t policy;
    mtfs_model_info_t info;
    uint8_t destination[5017];
    size_t loaded = 99U;
    env_init(&env, package, package_size, fleet_key);
    model_policy_init(&policy);
    memset(&info, 0x5A, sizeof(info));
    CHECK(mtfs_model_get_info(&env.model, &info) == MTFS_ERROR_INVALID_STATE);
    CHECK(all_value((const uint8_t *)&info, sizeof(info), 0x5AU));
    CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider, &env.work,
                          &policy) == MTFS_OK);
    CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider, &env.work,
                          &policy) == MTFS_ERROR_INVALID_STATE);
    CHECK(mtfs_model_get_info(&env.model, &info) == MTFS_OK);
    CHECK(info.api_version == MTFS_MODEL_INFO_API_VERSION &&
          info.struct_size == sizeof(info));
    CHECK(memcmp(info.model_id, package + 56U, sizeof(info.model_id)) == 0);
    CHECK(info.model_version == UINT64_C(0x0102030405060708));
    CHECK(info.target_id == MTFS_MODEL_TEST_TARGET_ID &&
          info.accelerator_id == MTFS_MODEL_TEST_ACCELERATOR_ID &&
          info.model_format == MTFS_MODEL_TEST_FORMAT_ID);
    CHECK(info.payload_size == payload_size && info.required_ram == payload_size);
    CHECK(info.chunk_size == 4096U && info.chunk_count == 2U);
    memset(destination, 0xA5, sizeof(destination));
    CHECK(mtfs_model_load(&env.model, destination, sizeof(destination),
                          &loaded) == MTFS_OK);
    CHECK(loaded == payload_size && memcmp(destination, payload, payload_size) == 0);
    CHECK(all_value(destination + payload_size,
                    sizeof(destination) - payload_size, 0xA5U));
    CHECK(mtfs_model_get_info(&env.model, &info) == MTFS_OK);
    CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    CHECK(memcmp(destination, payload, payload_size) == 0);
    return 1;
}

static int test_model_policy(const uint8_t *package, size_t package_size,
                             const uint8_t fleet_key[32])
{
    size_t i;
    for (i = 0U; i < 6U; ++i)
    {
        test_env_t env;
        mtfs_model_policy_t policy;
        mtfs_model_info_t info;
        mtfs_error_t result;
        env_init(&env, package, package_size, fleet_key);
        model_policy_init(&policy);
        if (i == 0U) policy.expected_target_id++;
        if (i == 1U) policy.expected_accelerator_id++;
        if (i == 2U) policy.accepted_model_format++;
        if (i == 3U) policy.maximum_chunk_size = 4095U;
        if (i == 4U) policy.maximum_model_payload_size = UINT64_C(4999);
        if (i == 5U) policy.maximum_required_ram = UINT64_C(4999);
        memset(&info, 0x5A, sizeof(info));
        result = mtfs_model_open(&env.model, &env.reader, &env.provider,
                                 &env.work, &policy);
        CHECK(result != MTFS_OK && env.model.state == MTFS_MODEL_ERROR);
        CHECK(env.memory.read_calls == 3U && env.crypto.decrypt_calls == 0U);
        CHECK(!env.crypto.fleet_open && !env.crypto.model_open);
        CHECK(mtfs_model_get_info(&env.model, &info) == MTFS_ERROR_INVALID_STATE);
        CHECK(all_value((const uint8_t *)&info, sizeof(info), 0x5AU));
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    }
    {
        test_env_t env;
        mtfs_model_policy_t policy;
        env_init(&env, package, package_size, fleet_key);
        model_policy_init(&policy);
        policy.expected_target_id = MTFS_MODEL_ID_INVALID;
        CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider,
                              &env.work, &policy) == MTFS_ERROR_INVALID_ARGUMENT);
        CHECK(env.memory.get_size_calls == 0U && env.memory.read_calls == 0U);
        CHECK(env.model.state == MTFS_MODEL_CLOSED);
    }
    return 1;
}

static int test_model_required_ram(const uint8_t *package, size_t package_size,
                                   const uint8_t fleet_key[32])
{
    uint8_t adjusted[5272];
    uint8_t destination[6017];
    test_env_t env;
    mtfs_model_policy_t policy;
    mtfs_model_info_t info;
    size_t loaded = 77U;
    CHECK(make_required_ram_package(package, package_size, fleet_key,
                                    UINT64_C(6000), adjusted));
    env_init(&env, adjusted, sizeof(adjusted), fleet_key);
    model_policy_init(&policy);
    policy.maximum_required_ram = UINT64_C(6000);
    CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider, &env.work,
                          &policy) == MTFS_OK);
    CHECK(mtfs_model_get_info(&env.model, &info) == MTFS_OK);
    CHECK(info.payload_size == 5000U && info.required_ram == 6000U);
    memset(destination, 0xA5, sizeof(destination));
    CHECK(mtfs_model_load(&env.model, destination, 5999U, &loaded) ==
          MTFS_ERROR_BUFFER_TOO_SMALL);
    CHECK(loaded == 0U && all_value(destination, sizeof(destination), 0xA5U));
    CHECK(env.memory.read_calls == 3U);
    CHECK(mtfs_model_load(&env.model, destination, 6000U, &loaded) == MTFS_OK);
    CHECK(loaded == 5000U && all_value(destination + 5000U, 1017U, 0xA5U));
    CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    return 1;
}

static int test_model_faults(const uint8_t *package, size_t package_size,
                             const uint8_t fleet_key[32])
{
    static const mtfs_error_t reader_errors[] = {
        MTFS_ERROR_NO_MEDIA, MTFS_ERROR_NOT_READY, MTFS_ERROR_OVERFLOW
    };
    size_t i;
    {
        test_env_t env;
        uint8_t destination[8];
        size_t loaded = 7U;
        env_init(&env, package, package_size, fleet_key);
        memset(destination, 0xA5, sizeof(destination));
        CHECK(mtfs_model_load(&env.model, destination, sizeof(destination),
                              &loaded) == MTFS_ERROR_INVALID_STATE);
        CHECK(loaded == 0U && all_value(destination, sizeof(destination), 0xA5U));
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    }
    for (i = 0U; i < sizeof(reader_errors) / sizeof(reader_errors[0]); ++i)
    {
        test_env_t env;
        mtfs_model_policy_t policy;
        env_init(&env, package, package_size, fleet_key);
        model_policy_init(&policy);
        env.memory.size_error = reader_errors[i];
        CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider,
                              &env.work, &policy) == reader_errors[i]);
        CHECK(env.model.state == MTFS_MODEL_ERROR);
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);

        env_init(&env, package, package_size, fleet_key);
        model_policy_init(&policy);
        env.memory.fail_read_call = 1U;
        env.memory.read_error = reader_errors[i];
        CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider,
                              &env.work, &policy) == reader_errors[i]);
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    }
    {
        test_env_t env;
        mtfs_model_policy_t policy;
        uint8_t destination[5017];
        size_t loaded = 55U;
        env_init(&env, package, package_size, fleet_key);
        model_policy_init(&policy);
        CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider,
                              &env.work, &policy) == MTFS_OK);
        memset(destination, 0xA5, sizeof(destination));
        CHECK(mtfs_model_load(&env.model, destination, 4999U, &loaded) ==
              MTFS_ERROR_BUFFER_TOO_SMALL);
        CHECK(loaded == 0U && all_value(destination, sizeof(destination), 0xA5U));
        CHECK(env.memory.read_calls == 3U && env.model.state == MTFS_MODEL_OPEN);
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    }
    for (i = 0U; i < 2U; ++i)
    {
        test_env_t env;
        mtfs_model_policy_t policy;
        mtfs_model_info_t info;
        uint8_t destination[5017];
        size_t loaded = 55U;
        mtfs_error_t expected = i == 0U ? MTFS_ERROR_NO_MEDIA : MTFS_ERROR_NOT_READY;
        env_init(&env, package, package_size, fleet_key);
        model_policy_init(&policy);
        CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider,
                              &env.work, &policy) == MTFS_OK);
        env.memory.fail_read_call = env.memory.read_calls + 2U;
        env.memory.read_error = expected;
        memset(destination, 0xA5, sizeof(destination));
        CHECK(mtfs_model_load(&env.model, destination, sizeof(destination),
                              &loaded) == expected);
        CHECK(loaded == 0U && all_value(destination, 5000U, 0U));
        CHECK(all_value(destination + 5000U, 17U, 0xA5U));
        CHECK(env.model.state == MTFS_MODEL_ERROR &&
              env.model.sealed_blob.state == MTFS_SEALED_BLOB_ERROR);
        CHECK(!env.crypto.fleet_open && !env.crypto.model_open);
        CHECK(all_value(env.manifest, sizeof(env.manifest), 0U));
        memset(&info, 0x5A, sizeof(info));
        CHECK(mtfs_model_get_info(&env.model, &info) == MTFS_ERROR_INVALID_STATE);
        CHECK(all_value((const uint8_t *)&info, sizeof(info), 0x5AU));
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    }
    {
        test_env_t env;
        mtfs_model_policy_t policy;
        uint8_t destination[5017];
        size_t loaded = 55U;
        env_init(&env, package, package_size, fleet_key);
        model_policy_init(&policy);
        env.crypto.fail_call = 1U;
        CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider,
                              &env.work, &policy) == MTFS_ERROR_CRYPTO);
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
        env_init(&env, package, package_size, fleet_key);
        model_policy_init(&policy);
        CHECK(mtfs_model_open(&env.model, &env.reader, &env.provider,
                              &env.work, &policy) == MTFS_OK);
        env.crypto.fail_call = 3U;
        memset(destination, 0xA5, sizeof(destination));
        CHECK(mtfs_model_load(&env.model, destination, sizeof(destination),
                              &loaded) == MTFS_ERROR_CRYPTO);
        CHECK(loaded == 0U && all_value(destination, 5000U, 0U));
        CHECK(all_value(destination + 5000U, 17U, 0xA5U));
        CHECK(mtfs_model_close(&env.model) == MTFS_OK);
    }
    return 1;
}

static int run_test(int (*test)(const uint8_t *, size_t, const uint8_t[32]),
                    const uint8_t *package, size_t size, const uint8_t key[32])
{
    ++tests;
    return test(package, size, key);
}

int main(int argc, char **argv)
{
    uint8_t *package;
    uint8_t *payload;
    uint8_t *key;
    size_t package_size;
    size_t payload_size;
    size_t key_size;
    int ok = 1;
    if (argc != 4)
        return 2;
    package = read_file(argv[1], &package_size);
    payload = read_file(argv[2], &payload_size);
    key = read_file(argv[3], &key_size);
    if (package == NULL || payload == NULL || key == NULL || key_size != 32U)
        return 2;
    ++tests;
    ok &= test_golden(package, package_size, payload, payload_size, key);
    ++tests;
    ok &= test_model_golden(package, package_size, payload, payload_size, key);
    ok &= run_test(test_mutations, package, package_size, key);
    ok &= run_test(test_layout_failures, package, package_size, key);
    ok &= run_test(test_faults_and_contracts, package, package_size, key);
    ok &= run_test(test_wrong_key, package, package_size, key);
    ok &= run_test(test_empty_payload, package, package_size, key);
    ok &= run_test(test_metadata_rules, package, package_size, key);
    ok &= run_test(test_layout_chunk_policy, package, package_size, key);
    ok &= run_test(test_model_policy, package, package_size, key);
    ok &= run_test(test_model_required_ram, package, package_size, key);
    ok &= run_test(test_model_faults, package, package_size, key);
    printf("sealed_blob: %u tests, %u checks, 30 mutations, 15 truncations, "
           "context=%zu info=%zu work=%zu model=%zu model_info=%zu policy=%zu\n",
           tests, checks,
           sizeof(mtfs_sealed_blob_t), sizeof(mtfs_sealed_package_info_t),
           sizeof(mtfs_sealed_work_t), sizeof(mtfs_model_t),
           sizeof(mtfs_model_info_t), sizeof(mtfs_model_policy_t));
    mtfs_secure_zero(key, key_size);
    free(key);
    free(payload);
    free(package);
    return ok ? 0 : 1;
}
