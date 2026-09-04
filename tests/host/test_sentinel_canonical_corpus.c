#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/sentinel/mtfs_sentinel_inference.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8U |
        (uint32_t)p[2] << 16U | (uint32_t)p[3] << 24U;
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4U) << 32U;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *bytes;
    long length;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    bytes = (uint8_t *)malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1U, (size_t)length, file) != (size_t)length) {
        free(bytes); fclose(file); return NULL;
    }
    fclose(file); *size = (size_t)length; return bytes;
}

int main(int argc, char **argv)
{
    uint8_t *bundle_bytes = NULL, *corpus = NULL;
    size_t bundle_size = 0U, corpus_size = 0U;
    mtfs_sentinel_bundle_policy_t policy;
    mtfs_sentinel_bundle_t bundle;
    mtfs_sentinel_cpu_context_t cpu;
    uint8_t work[MTFS_SENTINEL_CPU_WORK_SIZE];
    uint32_t count, record_size, index;
    int result_code = 1;
    if (argc != 3) {
        fprintf(stderr, "usage: %s BUNDLE CORPUS\n", argv[0]); return 2;
    }
    bundle_bytes = read_file(argv[1], &bundle_size);
    corpus = read_file(argv[2], &corpus_size);
    if (bundle_bytes == NULL || corpus == NULL || corpus_size < 16U ||
        memcmp(corpus, "MTFSCV1\0", 8U) != 0) {
        fprintf(stderr, "cannot load bundle/corpus\n"); goto done;
    }
    count = le32(corpus + 8U); record_size = le32(corpus + 12U);
    if (record_size != 112U || count > (corpus_size - 16U) / record_size ||
        corpus_size != 16U + (size_t)count * record_size) {
        fprintf(stderr, "invalid corpus framing\n"); goto done;
    }
    memset(&policy, 0, sizeof(policy));
    policy.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    policy.struct_size = (uint16_t)sizeof(policy);
    policy.expected_target_id = MTFS_SENTINEL_TARGET_STM32N6570_DK;
    policy.expected_transport_id = MTFS_SENTINEL_TRANSPORT_SDMMC_IDMA;
    policy.expected_accelerator_id = MTFS_SENTINEL_ACCELERATOR_NEURAL_ART;
    policy.expected_model_format = MTFS_SENTINEL_OUTER_MODEL_FORMAT_V1;
    {
        mtfs_error_t status = mtfs_sentinel_bundle_parse(bundle_bytes, bundle_size,
                                                          &policy, &bundle);
        if (status != MTFS_OK) {
            fprintf(stderr, "bundle parse failed: %d\n", (int)status); goto done;
        }
        status = mtfs_sentinel_cpu_init(&cpu, &bundle);
        if (status != MTFS_OK) {
            fprintf(stderr, "CPU init failed: %d\n", (int)status); goto done;
        }
    }
    for (index = 0U; index < count; ++index) {
        const uint8_t *record = corpus + 16U + (size_t)index * record_size;
        int8_t raw_output[24], q4_output[24];
        mtfs_sentinel_inference_result_t inference;
        if (mtfs_sentinel_cpu_infer_canonical_int8(&cpu,
                (const int8_t *)(record + 24U), work,
                sizeof(work), raw_output) != MTFS_OK ||
            memcmp(raw_output, record + 48U, 24U) != 0 ||
            mtfs_sentinel_cpu_infer(&cpu, (const int8_t *)record, work,
                sizeof(work), q4_output, &inference) != MTFS_OK ||
            memcmp(q4_output, record + 72U, 24U) != 0 ||
            inference.score_q8 != le64(record + 96U) ||
            inference.anomaly != record[104U]) {
            fprintf(stderr, "canonical corpus mismatch at vector %u\n", index);
            goto done;
        }
    }
    printf("canonical CPU/TFLite corpus: %u/%u bit-exact\n", count, count);
    result_code = 0;
done:
    memset(work, 0, sizeof(work)); free(corpus); free(bundle_bytes);
    return result_code;
}
