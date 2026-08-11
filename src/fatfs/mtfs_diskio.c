/* FatFs disk I/O bridge to the platform-independent microT-FS block API. */
#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "diskio.h"
#include "../block/mtfs_block_device.h"
#include "../block/mtfs_block_registry.h"

static DRESULT mtfs_disk_result(mtfs_error_t error)
{
    switch (error) {
    case MTFS_OK:
        return RES_OK;
    case MTFS_ERROR_WRITE_PROTECTED:
        return RES_WRPRT;
    case MTFS_ERROR_NOT_READY:
    case MTFS_ERROR_NO_MEDIA:
    case MTFS_ERROR_NOT_FOUND:
        return RES_NOTRDY;
    case MTFS_ERROR_INVALID_ARGUMENT:
    case MTFS_ERROR_OUT_OF_RANGE:
    case MTFS_ERROR_NOT_SUPPORTED:
        return RES_PARERR;
    case MTFS_ERROR_IO:
    case MTFS_ERROR_ALREADY_EXISTS:
    default:
        return RES_ERROR;
    }
}

static DSTATUS mtfs_disk_error_status(mtfs_error_t error)
{
    DSTATUS status = STA_NOINIT;

    if (error == MTFS_ERROR_NO_MEDIA) {
        status |= STA_NODISK;
    }
    if (error == MTFS_ERROR_WRITE_PROTECTED) {
        status |= STA_PROTECT;
    }
    return status;
}

static mtfs_error_t mtfs_disk_get_device(BYTE pdrv, mtfs_block_device_t **device)
{
    if ((uint32_t)pdrv >= (uint32_t)FF_VOLUMES) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    return mtfs_block_registry_get((uint32_t)pdrv, device);
}

static int mtfs_disk_convert_lba(LBA_t source, mtfs_lba_t *destination)
{
    mtfs_lba_t converted;

    if (destination == NULL) {
        return 0;
    }
    converted = (mtfs_lba_t)source;
    if ((LBA_t)converted != source) {
        return 0;
    }
    *destination = converted;
    return 1;
}

static int mtfs_disk_convert_count(UINT source, uint32_t *destination)
{
    uint32_t converted;

    if (destination == NULL) {
        return 0;
    }
    converted = (uint32_t)source;
    if ((UINT)converted != source) {
        return 0;
    }
    *destination = converted;
    return 1;
}

DSTATUS disk_status(BYTE pdrv)
{
    mtfs_block_device_t *device;
    mtfs_block_status_t block_status = 0U;
    mtfs_error_t result;
    DSTATUS status = 0U;

    result = mtfs_disk_get_device(pdrv, &device);
    if (result != MTFS_OK) {
        return mtfs_disk_error_status(result);
    }
    result = mtfs_block_status(device, &block_status);
    if (result != MTFS_OK) {
        return mtfs_disk_error_status(result);
    }
    if ((block_status & MTFS_BLOCK_STATUS_INITIALIZED) == 0U) {
        status |= STA_NOINIT;
    }
    if ((block_status & MTFS_BLOCK_STATUS_MEDIA_PRESENT) == 0U) {
        status |= (STA_NOINIT | STA_NODISK);
    }
    if ((block_status & MTFS_BLOCK_STATUS_WRITE_PROTECTED) != 0U) {
        status |= STA_PROTECT;
    }
    return status;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    mtfs_block_device_t *device;
    mtfs_error_t result;

    result = mtfs_disk_get_device(pdrv, &device);
    if (result != MTFS_OK) {
        return mtfs_disk_error_status(result);
    }
    result = mtfs_block_initialize(device);
    if (result != MTFS_OK) {
        return mtfs_disk_error_status(result);
    }
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    mtfs_block_device_t *device;
    mtfs_lba_t lba;
    uint32_t block_count;
    mtfs_error_t result;

    if ((buff == NULL) || (count == 0U) ||
        !mtfs_disk_convert_lba(sector, &lba) ||
        !mtfs_disk_convert_count(count, &block_count)) {
        return RES_PARERR;
    }
    result = mtfs_disk_get_device(pdrv, &device);
    if (result != MTFS_OK) {
        return mtfs_disk_result(result);
    }
    return mtfs_disk_result(mtfs_block_read(device, buff, lba, block_count));
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    mtfs_block_device_t *device;
    mtfs_lba_t lba;
    uint32_t block_count;
    mtfs_error_t result;

    if ((buff == NULL) || (count == 0U) ||
        !mtfs_disk_convert_lba(sector, &lba) ||
        !mtfs_disk_convert_count(count, &block_count)) {
        return RES_PARERR;
    }
    result = mtfs_disk_get_device(pdrv, &device);
    if (result != MTFS_OK) {
        return mtfs_disk_result(result);
    }
#if FF_FS_READONLY
    (void)device;
    (void)lba;
    return RES_WRPRT;
#else
    return mtfs_disk_result(mtfs_block_write(device, buff, lba, block_count));
#endif
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    mtfs_block_device_t *device;
    mtfs_block_geometry_t geometry;
    mtfs_error_t result;

    result = mtfs_disk_get_device(pdrv, &device);
    if (result != MTFS_OK) {
        return mtfs_disk_result(result);
    }

    if (cmd == CTRL_SYNC) {
        return mtfs_disk_result(mtfs_block_sync(device));
    }
    if (buff == NULL) {
        return RES_PARERR;
    }

    if ((cmd == GET_SECTOR_COUNT) || (cmd == GET_SECTOR_SIZE) ||
        (cmd == GET_BLOCK_SIZE)) {
        result = mtfs_block_get_geometry(device, &geometry);
        if (result != MTFS_OK) {
            return mtfs_disk_result(result);
        }
    }

    switch (cmd) {
    case GET_SECTOR_COUNT:
        if ((mtfs_lba_t)(LBA_t)geometry.sector_count != geometry.sector_count) {
            return RES_PARERR;
        }
        *(LBA_t *)buff = (LBA_t)geometry.sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (geometry.sector_size > UINT16_MAX) {
            return RES_PARERR;
        }
        *(WORD *)buff = (WORD)geometry.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = (DWORD)geometry.erase_block_size;
        if ((uint32_t)*(DWORD *)buff != geometry.erase_block_size) {
            return RES_PARERR;
        }
        return RES_OK;
    case CTRL_TRIM:
    {
        const LBA_t *range = (const LBA_t *)buff;
        mtfs_lba_t first;
        mtfs_lba_t last;
        mtfs_lba_t span;

        if (!mtfs_disk_convert_lba(range[0], &first) ||
            !mtfs_disk_convert_lba(range[1], &last) || (last < first)) {
            return RES_PARERR;
        }
        span = last - first;
        if (span == UINT64_MAX) {
            return RES_PARERR;
        }
        return mtfs_disk_result(mtfs_block_trim(device, first, span + 1U));
    }
    default:
        return RES_PARERR;
    }
}
