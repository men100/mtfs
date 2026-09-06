#ifndef MTFS_RA8P1_SENTINEL_INFERENCE_H
#define MTFS_RA8P1_SENTINEL_INFERENCE_H

#include <stdint.h>

#include "mtfs_media.h"
#include "mtfs_sentinel.h"
#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
#include "mtfs_sentinel_inference.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The caller owns the mounted FAT volume and registered block device. */
int mtfs_ra8p1_sentinel_inference_run(const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations);
int mtfs_ra8p1_sentinel_inference_profile_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations);
typedef mtfs_error_t (*mtfs_ra8p1_sentinel_media_transition_fn)(
    void *context, int present);
int mtfs_ra8p1_sentinel_inference_hotplug_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations,
    mtfs_ra8p1_sentinel_media_transition_fn transition,
    void *transition_context);
int mtfs_ra8p1_sentinel_inference_vector_run(
    const mtfs_media_context_t *media, const int8_t input_q4[24],
    uint32_t iterations);

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
mtfs_error_t mtfs_ra8p1_sentinel_monitor_open(void *media,
    uint64_t *threshold_q8);
mtfs_error_t mtfs_ra8p1_sentinel_monitor_normalize(void *media,
    const mtfs_sentinel_feature_v1_t *feature, int8_t input_q4[24]);
mtfs_error_t mtfs_ra8p1_sentinel_monitor_npu_infer(void *media,
    const int8_t input_q4[24], int8_t output_q4[24],
    mtfs_sentinel_inference_result_t *result, uint32_t *latency_us);
mtfs_error_t mtfs_ra8p1_sentinel_monitor_cpu_infer(void *media,
    const int8_t input_q4[24], mtfs_sentinel_inference_result_t *result,
    uint32_t *latency_us);
mtfs_error_t mtfs_ra8p1_sentinel_monitor_close(void *media);
#endif

#ifdef __cplusplus
}
#endif

#endif
