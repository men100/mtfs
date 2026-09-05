#ifndef MTFS_RA8P1_TFLM_SPIKE_H
#define MTFS_RA8P1_TFLM_SPIKE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mtfs_ra8p1_tflm_spike_run(uint32_t iterations, uint32_t heartbeat_before,
    volatile uint32_t *heartbeat);

#ifdef __cplusplus
}
#endif

#endif
