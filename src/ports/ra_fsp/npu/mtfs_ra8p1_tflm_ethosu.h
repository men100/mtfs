#ifndef MTFS_RA8P1_TFLM_ETHOSU_H
#define MTFS_RA8P1_TFLM_ETHOSU_H

#include "../../../sentinel/mtfs_sentinel_npu_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_RA8P1_ETHOSU_RUNTIME_ABI       (UINT32_C(0x00190200))
#define MTFS_RA8P1_ETHOSU_RUNTIME_VARIANT   (UINT32_C(256))
#define MTFS_RA8P1_ETHOSU_RUNTIME_EXTRA     (UINT32_C(0x00060500))
#define MTFS_RA8P1_ETHOSU_INTERPRETER_SIZE  (UINT32_C(224))
#define MTFS_RA8P1_ETHOSU_RESOLVER_SIZE     (UINT32_C(64))
#define MTFS_RA8P1_ETHOSU_ARENA_SIZE        (UINT32_C(4096))
#define MTFS_RA8P1_ETHOSU_CONTEXT_SIZE      (UINT32_C(512))
#define MTFS_RA8P1_ETHOSU_PERSISTENT_SIZE   (UINT32_C(4896))

typedef struct mtfs_ra8p1_tflm_ethosu_diagnostics
{
    uint32_t diagnostic_stage;
    int32_t diagnostic_detail;
    uint32_t diagnostic_expected;
    uint32_t diagnostic_actual;
    uint32_t model_bytes;
    uint32_t arena_used;
    uint32_t installs;
    uint32_t closes;
    uint32_t invokes;
    uint32_t invoke_failures;
    uint32_t cache_model_cleans;
    uint32_t global_opens;
    uint32_t global_open_failures;
} mtfs_ra8p1_tflm_ethosu_diagnostics_t;

typedef struct mtfs_ra8p1_tflm_ethosu
{
    void *session;
    mtfs_sentinel_npu_lock_fn lock;
    mtfs_sentinel_npu_unlock_fn unlock;
    void *callback_context;
    mtfs_ra8p1_tflm_ethosu_diagnostics_t diagnostics;
} mtfs_ra8p1_tflm_ethosu_t;

mtfs_error_t mtfs_ra8p1_tflm_ethosu_provider_config(
    mtfs_ra8p1_tflm_ethosu_t *target,
    mtfs_sentinel_npu_lock_fn lock,
    mtfs_sentinel_npu_unlock_fn unlock,
    void *callback_context,
    mtfs_sentinel_npu_provider_config_t *config);

/* Process-lifetime cleanup.  It must not be called while a model is open. */
mtfs_error_t mtfs_ra8p1_tflm_ethosu_global_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
