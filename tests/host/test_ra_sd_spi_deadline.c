#include "test_ra_sd_spi_deadline.h"

#include <stdint.h>

#include "mtfs_ra_sd_spi_deadline.h"
#include "mtfs_ra_sd_spi_protocol.h"

int test_ra_sd_spi_deadline(mtfs_test_t *test)
{
    mtfs_ra_sd_spi_deadline_t deadline;

    mtfs_ra_sd_spi_deadline_start(&deadline, UINT64_C(100), 10U);
    if (!MTFS_TEST_CHECK(test,
            !mtfs_ra_sd_spi_deadline_expired(&deadline, UINT64_C(109)),
            "deadline remains active immediately before boundary")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_deadline_expired(&deadline, UINT64_C(110)),
            "deadline expires exactly at boundary")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_deadline_expired(&deadline, UINT64_C(111)),
            "deadline remains expired after boundary")) {
        return 1;
    }

    mtfs_ra_sd_spi_deadline_start(&deadline, UINT64_MAX - UINT64_C(5), 10U);
    if (!MTFS_TEST_CHECK(test,
            !mtfs_ra_sd_spi_deadline_expired(&deadline, UINT64_C(3)),
            "deadline handles monotonic counter wrap before boundary")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_deadline_expired(&deadline, UINT64_C(4)),
            "deadline handles monotonic counter wrap at boundary")) {
        return 1;
    }

    mtfs_ra_sd_spi_deadline_start(&deadline, UINT64_MAX, 0U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_deadline_expired(&deadline, UINT64_MAX),
            "zero timeout expires immediately without addition overflow")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_cmd0_action(0x01U) ==
                MTFS_RA_SD_SPI_CMD0_ACCEPT,
            "CMD0 idle response is accepted")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_cmd0_action(0xFFU) ==
                MTFS_RA_SD_SPI_CMD0_RETRY_NO_RESPONSE,
            "CMD0 no-response byte is retried")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_cmd0_action(0x00U) ==
                MTFS_RA_SD_SPI_CMD0_RETRY_READY_RESPONSE,
            "CMD0 ready response is retried")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_cmd0_action(0x05U) ==
                MTFS_RA_SD_SPI_CMD0_REJECT_RESPONSE,
            "CMD0 response with error bits is rejected")) {
        return 1;
    }
    return 0;
}
