#ifndef MTFS_RA_SD_SPI_PROTOCOL_H
#define MTFS_RA_SD_SPI_PROTOCOL_H

#include <stdint.h>

typedef enum mtfs_ra_sd_spi_cmd0_action
{
    MTFS_RA_SD_SPI_CMD0_ACCEPT = 0,
    MTFS_RA_SD_SPI_CMD0_RETRY_NO_RESPONSE,
    MTFS_RA_SD_SPI_CMD0_RETRY_READY_RESPONSE,
    MTFS_RA_SD_SPI_CMD0_REJECT_RESPONSE
} mtfs_ra_sd_spi_cmd0_action_t;

static inline mtfs_ra_sd_spi_cmd0_action_t
mtfs_ra_sd_spi_cmd0_action(uint8_t r1)
{
    if (r1 == 0x01U) {
        return MTFS_RA_SD_SPI_CMD0_ACCEPT;
    }
    if (r1 == 0xFFU) {
        return MTFS_RA_SD_SPI_CMD0_RETRY_NO_RESPONSE;
    }
    if (r1 == 0x00U) {
        return MTFS_RA_SD_SPI_CMD0_RETRY_READY_RESPONSE;
    }
    return MTFS_RA_SD_SPI_CMD0_REJECT_RESPONSE;
}

#endif /* MTFS_RA_SD_SPI_PROTOCOL_H */
