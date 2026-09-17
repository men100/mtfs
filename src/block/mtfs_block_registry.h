/** @file mtfs_block_registry.h
 * @brief Fixed physical-drive registry used by the FatFs bridge. / FatFs bridgeが使用する固定サイズのphysical-drive registry。
 * @ingroup mtfs_block_registry */
#ifndef MTFS_BLOCK_REGISTRY_H
#define MTFS_BLOCK_REGISTRY_H

/** @addtogroup mtfs_block_registry
 * @{ */

#include <stdint.h>

#include "../mtfs_config.h"
#include "mtfs_block_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register one device without taking ownership. / deviceの所有権を取得せずregistryへ登録する。
 * @param physical_drive Index below MTFS_BLOCK_REGISTRY_SIZE. / MTFS_BLOCK_REGISTRY_SIZE未満のphysical drive index。
 * @param device Valid device kept alive until unregister. / unregisterするまで有効な状態を維持するdevice。
 * @retval MTFS_OK Registered. / 登録成功。
 * @retval MTFS_ERROR_ALREADY_EXISTS Slot occupied. / 指定されたslotはすでに使用中。
 * @return Argument or range error otherwise. / その他の場合は引数または範囲error。
 * @note The registry is not internally synchronized. / registry内部では同期を行わない。必要な同期は呼び出し側で行うこと。 */
mtfs_error_t mtfs_block_registry_register(uint32_t physical_drive, mtfs_block_device_t *device);

/** @brief Look up a registered device. / 登録済みdeviceを取得する。
 * @param physical_drive Physical-drive index. / physical drive index。
 * @param[out] device Borrowed pointer, set to NULL before a failed lookup. / 登録済みdeviceを参照する非所有pointerの出力先。lookupに失敗した場合はNULLを返す。
 * @return MTFS_OK, NOT_FOUND, OUT_OF_RANGE, or INVALID_ARGUMENT. / MTFS_OK、NOT_FOUND、OUT_OF_RANGE、またはINVALID_ARGUMENT。 */
mtfs_error_t mtfs_block_registry_get(uint32_t physical_drive, mtfs_block_device_t **device);

/** @brief Remove a registry entry. / registryからdeviceの登録を解除する。
 * @param physical_drive Physical-drive index. / physical drive index。
 * @return MTFS_OK, NOT_FOUND, or OUT_OF_RANGE. / MTFS_OK、NOT_FOUND、またはOUT_OF_RANGE。
 * @post The device is no longer reachable through diskio; its memory is not freed. / deviceはdiskioから参照できなくなるが、device自体のmemoryは解放されない。 */
mtfs_error_t mtfs_block_registry_unregister(uint32_t physical_drive);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MTFS_BLOCK_REGISTRY_H */
