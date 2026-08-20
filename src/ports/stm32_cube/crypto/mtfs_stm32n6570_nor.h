#ifndef MTFS_STM32N6570_NOR_H
#define MTFS_STM32N6570_NOR_H

#include "mtfs_stm32_nor_key_store.h"

int mtfs_stm32n6570_nor_open(mtfs_stm32_nor_io_t *io,
    int32_t *bsp_error);

#endif
