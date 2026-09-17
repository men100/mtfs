/** @file mtfs_block_device.h
 * @brief Platform-independent sector I/O API. / platform非依存のsector I/O API。
 * @ingroup mtfs_block_device */
#ifndef MTFS_BLOCK_DEVICE_H
#define MTFS_BLOCK_DEVICE_H

/** @addtogroup mtfs_block_device
 * @{ */

#include <stdint.h>

#include "../mtfs_config.h"
#include "../mtfs_error.h"
#include "../mtfs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t mtfs_block_status_t;
typedef uint32_t mtfs_block_capabilities_t;

#define MTFS_BLOCK_STATUS_INITIALIZED      (UINT32_C(1) << 0)
#define MTFS_BLOCK_STATUS_MEDIA_PRESENT    (UINT32_C(1) << 1)
#define MTFS_BLOCK_STATUS_WRITE_PROTECTED  (UINT32_C(1) << 2)

#define MTFS_BLOCK_CAPABILITY_READ_ONLY    (UINT32_C(1) << 0)
#define MTFS_BLOCK_CAPABILITY_TRIM         (UINT32_C(1) << 1)
#define MTFS_BLOCK_CAPABILITY_DIAGNOSTICS  (UINT32_C(1) << 2)

typedef struct mtfs_block_geometry
{
    uint32_t sector_size;
    mtfs_lba_t sector_count;
    uint32_t erase_block_size;
} mtfs_block_geometry_t;

/**
 * @brief Driver operation table. / block device driverのoperation table。
 * @details All callbacks run in caller task context. The driver owns context; it must remain valid while the device is registered or in use. read/write transfer exactly count complete sectors or return an error.
 * / すべてのcallbackは呼び出し側のtask contextで実行される。
 * contextはdriverが所有し、deviceがregistryへ登録されている間、または使用中は有効な状態を維持する必要がある。
 * read／writeは指定されたcount個のsectorをすべて転送するか、errorを返す。
 */
typedef struct mtfs_block_device_ops
{
    mtfs_error_t (*initialize)(void *context);
    mtfs_error_t (*status)(void *context, mtfs_block_status_t *status);
    mtfs_error_t (*read)(void *context, void *buffer, mtfs_lba_t lba, uint32_t count);
    mtfs_error_t (*write)(void *context, const void *buffer, mtfs_lba_t lba, uint32_t count);
    mtfs_error_t (*sync)(void *context);
    mtfs_error_t (*get_geometry)(void *context, mtfs_block_geometry_t *geometry);
    mtfs_error_t (*trim)(void *context, mtfs_lba_t lba, mtfs_lba_t count);
} mtfs_block_device_ops_t;

struct mtfs_block_device
{
    const mtfs_block_device_ops_t *ops;
    void *context;
    mtfs_block_capabilities_t capabilities;
#if MTFS_ENABLE_DIAGNOSTICS
    struct mtfs_block_diagnostics_state *diagnostics;
#endif
};

/** @brief Validate the operation/capability contract. / operationとcapabilityの組み合わせが有効か検証する。
 * @param device Device to inspect; ownership is unchanged. / 検証対象のdevice。所有権は移動しない。
 * @return Nonzero when usable, zero otherwise. / 使用可能な構成の場合は非0、それ以外は0。 */
int mtfs_block_device_is_valid(const mtfs_block_device_t *device);

/** @brief Initialize media access. / mediaへのアクセスを初期化する。
 * @param device Valid caller-owned device. / 呼び出し側が所有する有効なdevice。
 * @return MTFS_OK or the driver error; diagnostics record the attempt when enabled. / MTFS_OKまたはdriver error。diagnostics有効時は、このinitialize試行が記録される。 */
mtfs_error_t mtfs_block_initialize(mtfs_block_device_t *device);

/** @brief Read cached/live device status. / cache済みまたはdriverから取得したdevice statusを返す。
 * @param device Valid device. / 有効なdevice。
 * @param[out] status Status flags; read-only capability forces WRITE_PROTECTED. / status flagの出力先。READ_ONLY capabilityを持つdeviceではWRITE_PROTECTEDが必ず設定される。
 * @return MTFS_OK or an error. / MTFS_OKまたはerror。 */
mtfs_error_t mtfs_block_status(mtfs_block_device_t *device, mtfs_block_status_t *status);

/** @brief Read complete sectors. / sector単位でデータをreadする。
 * @param device Initialized device. / 初期化済みdevice。
 * @param[out] buffer Storage for count sectors; alignment follows the port contract. / count個のsectorを格納する出力buffer。alignment要件はport側のcontractに従う。
 * @param lba First sector. / 読み取り開始sectorのLBA。
 * @param count Nonzero sector count. / 読み取るsector数。0は指定不可。
 * @return MTFS_OK, range/argument error, or driver error. / MTFS_OK、範囲/引数error、またはdriver error。 */
mtfs_error_t mtfs_block_read(mtfs_block_device_t *device, void *buffer, mtfs_lba_t lba, uint32_t count);

/** @brief Write complete sectors. / sector単位でデータをwriteする。
 * @param device Initialized writable device. / 初期化済みかつ書き込み可能なdevice。
 * @param[in] buffer Source for count sectors; retained only during the call. / count個のsector分の入力buffer。参照されるのは関数呼び出し中のみ。
 * @param lba First sector. / 書き込み開始sectorのLBA。
 * @param count Nonzero sector count. / 書き込むsector数。0は指定不可。
 * @return MTFS_OK, WRITE_PROTECTED, range/argument error, or driver error. / MTFS_OK、WRITE_PROTECTED、範囲/引数error、またはdriver error。 */
mtfs_error_t mtfs_block_write(mtfs_block_device_t *device, const void *buffer, mtfs_lba_t lba, uint32_t count);

/** @brief Complete pending media writes. / 保留中のmedia writeを完了させる。
 * @param device Initialized device. / 初期化済みdevice。
 * @return MTFS_OK or driver error. / MTFS_OKまたはdriver error。 */
mtfs_error_t mtfs_block_sync(mtfs_block_device_t *device);

/** @brief Obtain nonzero geometry. / すべての必須fieldが0以外のdevice geometryを取得する。
 * @param device Valid device. / 有効なdevice。
 * @param[out] geometry Sector size/count and erase granularity. / sector size、sector count、およびerase単位の出力先。
 * @return MTFS_OK or an error; zero fields are rejected. / MTFS_OKまたはerror。必須fieldが0の場合はerrorとなる。 */
mtfs_error_t mtfs_block_get_geometry(mtfs_block_device_t *device, mtfs_block_geometry_t *geometry);

/** @brief Discard an address range when supported. / 対応しているdeviceで指定sector範囲をdiscardする。
 * @param device Initialized device with TRIM capability. / TRIM capabilityを持つ初期化済みdevice。
 * @param lba First sector. / 対象範囲の先頭sectorのLBA。
 * @param count Nonzero number of sectors. / discardするsector数。0は指定不可。
 * @return MTFS_OK, NOT_SUPPORTED, WRITE_PROTECTED, range error, or driver error. / MTFS_OK、NOT_SUPPORTED、WRITE_PROTECTED、範囲error、またはdriver error。 */
mtfs_error_t mtfs_block_trim(mtfs_block_device_t *device, mtfs_lba_t lba, mtfs_lba_t count);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MTFS_BLOCK_DEVICE_H */
