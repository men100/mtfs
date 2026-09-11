#ifndef MTFS_RA8P1_BOARD_CONFIG_H
#define MTFS_RA8P1_BOARD_CONFIG_H

#include "bsp_pin_cfg.h"

/*
 * Only the GPIOs driven or sampled directly by the microT-FS board port are
 * named here. SCI0 SCK/RXD/MISO and TXD/MOSI pinmux remains FSP-generated;
 * change it in the target configuration.xml, not by editing this header.
 * microT-FSのboard portが直接制御するGPIOだけをここで定義する。SCI0の
 * SCK/RXD(MISO)/TXD(MOSI) pinmuxはFSP生成設定であり、このheaderではなく
 * targetのconfiguration.xmlで変更する。
 */
#define MTFS_RA8P1_SD_CHIP_SELECT_PIN PMOD2_CTS
#define MTFS_RA8P1_SD_CARD_DETECT_PIN PMOD2_GPIO1
#define MTFS_RA8P1_SD_CARD_DETECT_ACTIVE_LOW (1U)
#define MTFS_RA8P1_SD_CARD_DETECT_DEBOUNCE_MS (100U)

/*
 * FSP binds P409 to external IRQ channel 6. P000 must remain a plain input so
 * it does not compete for IRQ6; configuration.xml is the source of truth.
 * FSPはP409を外部IRQ channel 6へ割り当てる。IRQ6競合を避けるためP000は
 * 通常入力のままとし、configuration.xmlを正とする。
 */
#define MTFS_RA8P1_SD_CARD_DETECT_IRQ_CHANNEL (6U)

#endif /* MTFS_RA8P1_BOARD_CONFIG_H */
