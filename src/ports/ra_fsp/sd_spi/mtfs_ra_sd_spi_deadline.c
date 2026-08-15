#include "mtfs_ra_sd_spi_deadline.h"

void mtfs_ra_sd_spi_deadline_start(
    mtfs_ra_sd_spi_deadline_t *deadline,
    uint64_t now_ms,
    uint32_t timeout_ms)
{
    deadline->start_ms = now_ms;
    deadline->timeout_ms = timeout_ms;
}

int mtfs_ra_sd_spi_deadline_expired(
    const mtfs_ra_sd_spi_deadline_t *deadline,
    uint64_t now_ms)
{
    /* Unsigned subtraction remains valid when the monotonic counter wraps. */
    return (uint64_t)(now_ms - deadline->start_ms) >= deadline->timeout_ms;
}
