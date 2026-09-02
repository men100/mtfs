#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtfs_sentinel_inference.h"

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX)
        return 0;
    *value = (uint32_t)parsed;
    return 1;
}

int main(int argc, char **argv)
{
    FILE *stream;
    long length;
    uint8_t *bytes = NULL;
    mtfs_sentinel_bundle_policy_t policy;
    mtfs_sentinel_bundle_t bundle;
    mtfs_sentinel_cpu_context_t cpu;
    mtfs_sentinel_inference_result_t result;
    int8_t input[24], output[24], work[48];
    int status = 2, i;
    if (argc != 29) {
        fprintf(stderr, "usage: c_reference_runner BUNDLE TARGET TRANSPORT ACCEL Q4[24]\n");
        return 2;
    }
    (void)memset(&policy, 0, sizeof(policy));
    policy.api_version = 1U; policy.struct_size = (uint16_t)sizeof(policy);
    policy.expected_model_format = UINT32_C(0x534e5431);
    policy.maximum_bundle_size = MTFS_SENTINEL_BUNDLE_MAX_SIZE;
    if (!parse_u32(argv[2], &policy.expected_target_id) ||
        !parse_u32(argv[3], &policy.expected_transport_id) ||
        !parse_u32(argv[4], &policy.expected_accelerator_id)) return 2;
    for (i = 0; i < 24; ++i) {
        char *end = NULL;
        long value;
        errno = 0; value = strtol(argv[5 + i], &end, 10);
        if (errno != 0 || end == argv[5 + i] || *end != '\0' ||
            value < -128 || value > 127) return 2;
        input[i] = (int8_t)value;
    }
    stream = fopen(argv[1], "rb");
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (length = ftell(stream)) <= 0L || fseek(stream, 0L, SEEK_SET) != 0)
        goto cleanup;
    bytes = (uint8_t *)malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1U, (size_t)length, stream) != (size_t)length)
        goto cleanup;
    if (mtfs_sentinel_bundle_parse(bytes, (size_t)length, &policy, &bundle) != MTFS_OK ||
        mtfs_sentinel_cpu_init(&cpu, &bundle) != MTFS_OK ||
        mtfs_sentinel_cpu_infer(&cpu, input, work, sizeof(work), output, &result) != MTFS_OK)
        goto cleanup;
    printf("{\"score_q8\":%llu,\"threshold_q8\":%llu,\"anomaly\":%u,\"output_q4\":[",
        (unsigned long long)result.score_q8,
        (unsigned long long)result.threshold_q8, (unsigned)result.anomaly);
    for (i = 0; i < 24; ++i) printf("%s%d", i == 0 ? "" : ",", (int)output[i]);
    printf("]}\n");
    status = 0;
cleanup:
    if (stream != NULL) (void)fclose(stream);
    free(bytes);
    return status;
}
