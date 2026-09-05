#ifndef MTFS_RA8P1_ETHOSU_HOOKS_H
#define MTFS_RA8P1_ETHOSU_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mtfs_ra8p1_ethosu_hook_diagnostics {
    uint32_t creates;
    uint32_t create_failures;
    uint32_t destroys;
    uint32_t rejected_destroys;
    uint32_t takes;
    uint32_t gives;
    uint32_t rejected_gives;
    uint32_t wfe_calls;
    uint32_t timeouts;
    uint32_t peak_in_use;
    uint32_t active;
    uint64_t wait_us;
} mtfs_ra8p1_ethosu_hook_diagnostics_t;

void mtfs_ra8p1_ethosu_hook_diagnostics_reset(void);
void mtfs_ra8p1_ethosu_hook_diagnostics_get(
    mtfs_ra8p1_ethosu_hook_diagnostics_t *diagnostics);

#ifdef __cplusplus
}
#endif
#endif
