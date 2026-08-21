#include <stddef.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "mtfs_block_device.h"
#include "mtfs_block_registry.h"
#include "mtfs_media.h"
#include "mtfs_media_service.h"
#include "mtfs_simple.h"
#include "mtfs_stm32_sdmmc.h"
#include "mtfs_stm32n6570_dk_platform.h"

static mtfs_stm32_sdmmc_context_t sd_context;
static mtfs_media_context_t media_context;
static mtfs_media_service_context_t media_service;

static int run_simple_application(void)
{
    mtfs_stm32_sdmmc_config_t config;
    mtfs_block_device_t *device = NULL;
    mtfs_error_t error;
    int context_ready = 0;
    int media_ready = 0;
    int registered = 0;
    int failed = 1;

    mtfs_stm32n6570_dk_sdmmc_config(&config);
    error = mtfs_stm32_sdmmc_context_init(&sd_context, &config);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[simple] SD context init FAIL mtfs=%d\n", error);
        goto cleanup;
    }
    context_ready = 1;

    error = mtfs_stm32n6570_dk_card_detect_start(&media_context,
        &media_service, &sd_context, NULL, NULL);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[simple] card detect start FAIL mtfs=%d\n",
            error);
        goto cleanup;
    }
    media_ready = 1;

    device = mtfs_stm32_sdmmc_block_device(&sd_context);
    error = mtfs_block_initialize(device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[simple] SD initialize FAIL mtfs=%d\n", error);
        goto cleanup;
    }
    error = mtfs_block_registry_register(0U, device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[simple] drive register FAIL mtfs=%d\n", error);
        goto cleanup;
    }
    registered = 1;
    failed = mtfs_simple_run();

cleanup:
    if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
        tm_printf((UB *)"[simple] drive unregister FAIL\n");
        failed = 1;
    }
    if (media_ready &&
        (mtfs_stm32n6570_dk_card_detect_stop() != MTFS_OK)) {
        tm_printf((UB *)"[simple] card detect stop FAIL\n");
        failed = 1;
    }
    if (context_ready &&
        (mtfs_stm32_sdmmc_context_deinit(&sd_context) != MTFS_OK)) {
        tm_printf((UB *)"[simple] SD context deinit FAIL\n");
        failed = 1;
    }
    return failed;
}

static void simple_task(INT start_code, void *context)
{
    int failed;
    (void)start_code;
    (void)context;

    tm_printf((UB *)"\nmicroT-FS simple FatFs sample (STM32N6570-DK)\n");
    tm_printf((UB *)"[simple] insert a FAT-formatted SD card\n");
    failed = run_simple_application();
    tm_printf((UB *)"[simple] application %s\n",
        failed ? (UB *)"FAIL" : (UB *)"PASS");
    tk_exd_tsk();
}

EXPORT INT usermain(void)
{
    T_CTSK task = {
        .tskatr = TA_HLNG | TA_RNG3,
        .task = simple_task,
        .itskpri = 9,
        .stksz = 8U * 1024U
    };
    ID task_id = tk_cre_tsk(&task);

    if ((task_id <= 0) || (tk_sta_tsk(task_id, 0) < E_OK)) {
        tm_printf((UB *)"[simple] task start FAIL\n");
    }
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
    return 0;
}
