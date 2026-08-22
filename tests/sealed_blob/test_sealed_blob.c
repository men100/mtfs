#include <openssl/evp.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtfs_sealed_blob.h"
#include "mtfs_secure_zero.h"

typedef struct memory_reader
{
    const uint8_t *data;
    size_t size;
    unsigned get_size_calls;
    unsigned read_calls;
    unsigned fail_read_call;
    unsigned short_read_call;
    int fail_size;
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
    if (reader->fail_size)
        return MTFS_ERROR_IO;
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
        return MTFS_ERROR_IO;
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
    env.memory.fail_size = 1;
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
    ok &= run_test(test_mutations, package, package_size, key);
    ok &= run_test(test_layout_failures, package, package_size, key);
    ok &= run_test(test_faults_and_contracts, package, package_size, key);
    ok &= run_test(test_wrong_key, package, package_size, key);
    ok &= run_test(test_empty_payload, package, package_size, key);
    ok &= run_test(test_metadata_rules, package, package_size, key);
    ok &= run_test(test_layout_chunk_policy, package, package_size, key);
    printf("sealed_blob: %u tests, %u checks, 30 mutations, 15 truncations, "
           "context=%zu info=%zu work=%zu\n", tests, checks,
           sizeof(mtfs_sealed_blob_t), sizeof(mtfs_sealed_package_info_t),
           sizeof(mtfs_sealed_work_t));
    mtfs_secure_zero(key, key_size);
    free(key);
    free(payload);
    free(package);
    return ok ? 0 : 1;
}
