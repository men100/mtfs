#ifndef MTFS_RA_SD_SPI_DEADLINE_H
#define MTFS_RA_SD_SPI_DEADLINE_H

#include <stdint.h>

typedef struct mtfs_ra_sd_spi_deadline
{
    uint64_t start_ms;
    uint64_t timeout_ms;
} mtfs_ra_sd_spi_deadline_t;

void mtfs_ra_sd_spi_deadline_start(
    mtfs_ra_sd_spi_deadline_t *deadline,
    uint64_t now_ms,
    uint32_t timeout_ms);

int mtfs_ra_sd_spi_deadline_expired(
    const mtfs_ra_sd_spi_deadline_t *deadline,
    uint64_t now_ms);

#endif /* MTFS_RA_SD_SPI_DEADLINE_H */
