#include "mtfs_stm32n6570_nor.h"

#include "stm32n6570_discovery_xspi.h"

static int nor_open;

static int nor_read(void *context, uint32_t offset, uint8_t *data,
    size_t bytes)
{
    (void)context;
    return (int)BSP_XSPI_NOR_Read(0U, data, offset, (uint32_t)bytes);
}

static int nor_erase(void *context, uint32_t offset)
{
    (void)context;
    return (int)BSP_XSPI_NOR_Erase_Block(0U, offset,
        BSP_XSPI_NOR_ERASE_4K);
}

static int nor_program(void *context, uint32_t offset, const uint8_t *data,
    size_t bytes)
{
    (void)context;
    return (int)BSP_XSPI_NOR_Write(0U, data, offset, (uint32_t)bytes);
}

int mtfs_stm32n6570_nor_open(mtfs_stm32_nor_io_t *io,
    int32_t *bsp_error)
{
    BSP_XSPI_NOR_Info_t info;
    BSP_XSPI_NOR_Init_t init = {
        BSP_XSPI_NOR_OPI_MODE,
        BSP_XSPI_NOR_DTR_TRANSFER
    };
    int32_t error = BSP_ERROR_NONE;
    if (io == NULL) return -1;
    if (!nor_open) {
        error = BSP_XSPI_NOR_Init(0U, &init);
        if (error == BSP_ERROR_NONE) error = BSP_XSPI_NOR_GetInfo(0U, &info);
        if ((error == BSP_ERROR_NONE) &&
            ((info.FlashSize != MTFS_STM32_NOR_BYTES) ||
             (info.EraseSubSectorSize != MTFS_STM32_NOR_ERASE_BYTES) ||
             (info.ProgPageSize != MTFS_STM32_NOR_PROGRAM_BYTES))) {
            error = BSP_ERROR_COMPONENT_FAILURE;
        }
        if (error == BSP_ERROR_NONE) nor_open = 1;
    }
    if (bsp_error != NULL) *bsp_error = error;
    if (!nor_open) return -1;
    io->context = NULL;
    io->read = nor_read;
    io->erase_sector = nor_erase;
    io->program = nor_program;
    return 0;
}
