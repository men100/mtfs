#ifndef MTFS_STM32N6570_SENTINEL_INFERENCE_H
#define MTFS_STM32N6570_SENTINEL_INFERENCE_H

#include <stdint.h>

#include "mtfs_media.h"
#include "mtfs_sentinel.h"

/* The caller owns the mounted FAT volume and registered block device. */
int mtfs_stm32n6570_sentinel_inference_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations);
int mtfs_stm32n6570_sentinel_inference_hotplug_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations);

#endif
