#ifndef MTFS_STM32N6570_DK_BOARD_CONFIG_H
#define MTFS_STM32N6570_DK_BOARD_CONFIG_H

#include "main.h"

/*
 * These aliases cover GPIO/IRQ values consumed directly by the microT-FS
 * board port. SDMMC2 alternate-function pins are Cube-generated from the
 * target .ioc and cannot be redirected by changing these macros alone.
 * これらのaliasはmicroT-FS board portが直接参照するGPIO/IRQだけを扱う。
 * SDMMC2 alternate-function pinはtargetの.iocからCube生成されるため、
 * このmacroだけを変更して配線先を変えることはできない。
 */
#define MTFS_STM32N6570_DK_SD_CARD_DETECT_PIN SD_DETECT_Pin
#define MTFS_STM32N6570_DK_SD_CARD_DETECT_PORT SD_DETECT_GPIO_Port
#define MTFS_STM32N6570_DK_SD_CARD_DETECT_IRQ EXTI12_IRQn
#define MTFS_STM32N6570_DK_SD_CARD_DETECT_ACTIVE_LOW (1U)
#define MTFS_STM32N6570_DK_SD_CARD_DETECT_DEBOUNCE_MS (500U)
#define MTFS_STM32N6570_DK_SD_CARD_DETECT_IRQ_PRIORITY (6U)

/* The on-board microSD socket does not expose a write-protect input. */
/* board上のmicroSD socketにはwrite-protect入力が接続されていない。 */
#define MTFS_STM32N6570_DK_SD_HAS_WRITE_PROTECT (0U)

#endif /* MTFS_STM32N6570_DK_BOARD_CONFIG_H */
