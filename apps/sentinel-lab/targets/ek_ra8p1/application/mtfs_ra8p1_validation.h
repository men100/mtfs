#ifndef MTFS_RA8P1_VALIDATION_H
#define MTFS_RA8P1_VALIDATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pre-freeze validation-only runner.  It never opens the held-out corpus. */
int mtfs_ra8p1_validation_run(uint32_t repeats);

#ifdef __cplusplus
}
#endif

#endif
