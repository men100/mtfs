#include "mtfs_ra_sd_spi.h"
#include "mtfs_ra_sd_spi_deadline.h"

#include <stddef.h>
#include <string.h>

#define MTFS_SD_CMD0    (0U)
#define MTFS_SD_CMD8    (8U)
#define MTFS_SD_CMD9    (9U)
#define MTFS_SD_CMD17   (17U)
#define MTFS_SD_CMD24   (24U)
#define MTFS_SD_CMD55   (55U)
#define MTFS_SD_CMD58   (58U)
#define MTFS_SD_ACMD41  (41U)

#define MTFS_SD_R1_IDLE             (0x01U)
#define MTFS_SD_R1_ILLEGAL_COMMAND  (0x04U)
#define MTFS_SD_DATA_TOKEN          (0xFEU)
#define MTFS_SD_DATA_RESPONSE_MASK  (0x1FU)
#define MTFS_SD_DATA_ACCEPTED       (0x05U)
#define MTFS_SD_EVENT_TRANSFER      (UINT32_C(1) << 0)
#define MTFS_SD_EVENT_REMOVED       (UINT32_C(1) << 1)
#define MTFS_SD_COMMAND_POLLS       (8U)
#define MTFS_SD_ERASE_BLOCK_SECTORS (1U)
#define MTFS_SD_ACMD41_RETRY_DELAY_MS (1U)

static mtfs_error_t mtfs_sd_initialize(void *opaque);
static mtfs_error_t mtfs_sd_status(void *opaque, mtfs_block_status_t *status);
static mtfs_error_t mtfs_sd_read(void *opaque, void *buffer, mtfs_lba_t lba, uint32_t count);
static mtfs_error_t mtfs_sd_write(void *opaque, const void *buffer, mtfs_lba_t lba, uint32_t count);
static mtfs_error_t mtfs_sd_sync(void *opaque);
static mtfs_error_t mtfs_sd_geometry(void *opaque, mtfs_block_geometry_t *geometry);
static mtfs_error_t mtfs_sd_trim(void *opaque, mtfs_lba_t lba, mtfs_lba_t count);

static const mtfs_block_device_ops_t mtfs_sd_ops = {
    mtfs_sd_initialize,
    mtfs_sd_status,
    mtfs_sd_read,
    mtfs_sd_write,
    mtfs_sd_sync,
    mtfs_sd_geometry,
    mtfs_sd_trim
};

static mtfs_error_t mtfs_sd_set_error(
    mtfs_ra_sd_spi_context_t *context, mtfs_error_t error)
{
    context->last_error = error;
    return error;
}

static int mtfs_sd_card_present(const mtfs_ra_sd_spi_context_t *context)
{
    return (context->config.card_present == NULL) ||
        context->config.card_present(context->config.signal_context);
}

static void mtfs_sd_invalidate_media(mtfs_ra_sd_spi_context_t *context)
{
    context->initialized = 0U;
    context->card_type = MTFS_RA_SD_CARD_UNKNOWN;
    context->sector_count = 0U;
}

static mtfs_error_t mtfs_sd_no_media(mtfs_ra_sd_spi_context_t *context)
{
    mtfs_sd_invalidate_media(context);
    return mtfs_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
}

static mtfs_error_t mtfs_sd_kernel_error(mtfs_ra_sd_spi_context_t *context, ER error)
{
    context->last_kernel_error = error;
    if (error >= E_OK) {
        return mtfs_sd_set_error(context, MTFS_OK);
    }
    switch (MERCD(error)) {
    case MERCD(E_PAR):
    case MERCD(E_ID):
        return mtfs_sd_set_error(context, MTFS_ERROR_INVALID_ARGUMENT);
    case MERCD(E_NOSPT):
        return mtfs_sd_set_error(context, MTFS_ERROR_NOT_SUPPORTED);
    case MERCD(E_NOEXS):
    case MERCD(E_OBJ):
    case MERCD(E_BUSY):
    case MERCD(E_TMOUT):
        return mtfs_sd_set_error(context, MTFS_ERROR_NOT_READY);
    case MERCD(E_NOMDA):
        return mtfs_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
    case MERCD(E_RONLY):
        return mtfs_sd_set_error(context, MTFS_ERROR_WRITE_PROTECTED);
    default:
        return mtfs_sd_set_error(context, MTFS_ERROR_IO);
    }
}

static void mtfs_sd_diagnostic_increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX) {
        ++*counter;
    }
}

static void mtfs_sd_diagnostic_record_polls(
    uint32_t *maximum, uint32_t poll_count)
{
    if (poll_count > *maximum) {
        *maximum = poll_count;
    }
}

static mtfs_error_t mtfs_sd_monotonic_ms(
    mtfs_ra_sd_spi_context_t *context, uint64_t *milliseconds)
{
    SYSTIM time;
    ER result = tk_get_otm(&time);

    if (result < E_OK) {
        mtfs_sd_diagnostic_increment(
            &context->diagnostics.monotonic_clock_errors);
        return mtfs_sd_kernel_error(context, result);
    }
    *milliseconds = ((uint64_t)(uint32_t)time.hi << 32U) | time.lo;
    return MTFS_OK;
}

static mtfs_error_t mtfs_sd_deadline_start(
    mtfs_ra_sd_spi_context_t *context,
    uint32_t timeout_ms,
    mtfs_ra_sd_spi_deadline_t *deadline)
{
    uint64_t now_ms = 0U;
    mtfs_error_t result = mtfs_sd_monotonic_ms(context, &now_ms);

    if (result == MTFS_OK) {
        mtfs_ra_sd_spi_deadline_start(deadline, now_ms, timeout_ms);
    }
    return result;
}

static mtfs_error_t mtfs_sd_deadline_expired(
    mtfs_ra_sd_spi_context_t *context,
    const mtfs_ra_sd_spi_deadline_t *deadline,
    int *expired)
{
    uint64_t now_ms = 0U;
    mtfs_error_t result = mtfs_sd_monotonic_ms(context, &now_ms);

    if (result == MTFS_OK) {
        *expired = mtfs_ra_sd_spi_deadline_expired(deadline, now_ms);
    }
    return result;
}

static mtfs_error_t mtfs_sd_fsp_error(mtfs_ra_sd_spi_context_t *context, fsp_err_t error)
{
    context->last_fsp_error = error;
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    if (error == FSP_SUCCESS) {
        return mtfs_sd_set_error(context, MTFS_OK);
    }
    if (error == FSP_ERR_ASSERTION || error == FSP_ERR_INVALID_ARGUMENT) {
        return mtfs_sd_set_error(context, MTFS_ERROR_INVALID_ARGUMENT);
    }
    if (error == FSP_ERR_NOT_OPEN || error == FSP_ERR_IN_USE || error == FSP_ERR_TIMEOUT) {
        return mtfs_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    return mtfs_sd_set_error(context, MTFS_ERROR_IO);
}

static ER mtfs_sd_driver_open(ID device_id, UINT open_mode, void *opaque)
{
    mtfs_ra_sd_spi_context_t *context = (mtfs_ra_sd_spi_context_t *)opaque;
    fsp_err_t result;

    (void)device_id;
    (void)open_mode;
    if ((context == NULL) || (context->config.spi == NULL)) {
        return E_PAR;
    }
    if (context->fsp_open != 0U) {
        return E_OK;
    }
    result = R_SCI_B_SPI_Open(context->config.spi->p_ctrl, &context->spi_config_ram);
    context->last_fsp_error = result;
    if (result != FSP_SUCCESS) {
        return E_IO;
    }
    context->fsp_open = 1U;
    result = R_SCI_B_SPI_CallbackSet(
        context->config.spi->p_ctrl, mtfs_ra_sd_spi_callback, context, NULL);
    context->last_fsp_error = result;
    if (result != FSP_SUCCESS) {
        (void)R_SCI_B_SPI_Close(context->config.spi->p_ctrl);
        context->fsp_open = 0U;
        return E_IO;
    }
    return E_OK;
}

static ER mtfs_sd_driver_close(ID device_id, UINT option, void *opaque)
{
    mtfs_ra_sd_spi_context_t *context = (mtfs_ra_sd_spi_context_t *)opaque;
    fsp_err_t result;

    (void)device_id;
    (void)option;
    if ((context == NULL) || (context->fsp_open == 0U)) {
        return E_OK;
    }
    result = R_SCI_B_SPI_Close(context->config.spi->p_ctrl);
    context->last_fsp_error = result;
    if (result != FSP_SUCCESS) {
        return E_IO;
    }
    context->fsp_open = 0U;
    return E_OK;
}

static ER mtfs_sd_driver_execute(T_DEVREQ *request, TMO timeout, void *opaque)
{
    (void)timeout;
    (void)opaque;
    if (request == NULL) {
        return E_PAR;
    }
    request->asize = 0;
    request->error = E_NOSPT;
    return E_OK;
}

static INT mtfs_sd_driver_wait(T_DEVREQ *request, INT request_count, TMO timeout, void *opaque)
{
    (void)request;
    (void)request_count;
    (void)timeout;
    (void)opaque;
    return 0;
}

static ER mtfs_sd_driver_abort(ID task_id, T_DEVREQ *request, INT request_count, void *opaque)
{
    (void)task_id;
    (void)request;
    (void)request_count;
    (void)opaque;
    return E_OK;
}

static INT mtfs_sd_driver_event(INT event_type, void *event_info, void *opaque)
{
    (void)event_type;
    (void)event_info;
    (void)opaque;
    return E_NOSPT;
}

void mtfs_ra_sd_spi_callback(spi_callback_args_t *args)
{
    mtfs_ra_sd_spi_context_t *context;

    if ((args == NULL) || (args->p_context == NULL)) {
        return;
    }
    context = (mtfs_ra_sd_spi_context_t *)args->p_context;
    context->transfer_result =
        (args->event == SPI_EVENT_TRANSFER_COMPLETE) ? E_OK : E_IO;
    if (context->transfer_result == E_OK) {
        ++context->diagnostics.transfer_completions;
    } else {
        ++context->diagnostics.transfer_errors;
    }
    (void)tk_set_flg(context->transfer_event_flag_id, MTFS_SD_EVENT_TRANSFER);
}

static mtfs_error_t mtfs_sd_lock(mtfs_ra_sd_spi_context_t *context)
{
    return mtfs_sd_kernel_error(context, tk_loc_mtx(
        context->access_mutex_id, (TMO)context->config.transfer_timeout_ms));
}

static void mtfs_sd_unlock(mtfs_ra_sd_spi_context_t *context)
{
    ER result = tk_unl_mtx(context->access_mutex_id);
    if (result < E_OK) {
        context->last_kernel_error = result;
        context->last_error = MTFS_ERROR_IO;
    }
}

static mtfs_error_t mtfs_sd_cs(mtfs_ra_sd_spi_context_t *context, bsp_io_level_t level)
{
    fsp_err_t result = context->config.ioport->p_api->pinWrite(
        context->config.ioport->p_ctrl, context->config.chip_select_pin, level);
    return mtfs_sd_fsp_error(context, result);
}

static mtfs_error_t mtfs_sd_transfer(
    mtfs_ra_sd_spi_context_t *context,
    const uint8_t *transmit,
    uint8_t *receive,
    uint32_t length)
{
    UINT flags;
    fsp_err_t fsp_result;
    ER kernel_result;

    if ((length == 0U) || (length > MTFS_RA_SD_SPI_DUMMY_SIZE) ||
        ((transmit == NULL) && (receive == NULL))) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    if (context->media_removal_pending) {
        return mtfs_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    kernel_result = tk_clr_flg(context->transfer_event_flag_id,
        ~(MTFS_SD_EVENT_TRANSFER | MTFS_SD_EVENT_REMOVED));
    if (kernel_result < E_OK) {
        return mtfs_sd_kernel_error(context, kernel_result);
    }
    context->transfer_result = E_IO;
    ++context->diagnostics.transfer_starts;
    if ((transmit != NULL) && (receive != NULL)) {
        fsp_result = R_SCI_B_SPI_WriteRead(context->config.spi->p_ctrl,
            transmit, receive, length, SPI_BIT_WIDTH_8_BITS);
    } else if (transmit != NULL) {
        fsp_result = R_SCI_B_SPI_Write(context->config.spi->p_ctrl,
            transmit, length, SPI_BIT_WIDTH_8_BITS);
    } else {
        fsp_result = R_SCI_B_SPI_WriteRead(context->config.spi->p_ctrl,
            context->dummy_tx, receive, length, SPI_BIT_WIDTH_8_BITS);
    }
    if (fsp_result != FSP_SUCCESS) {
        return mtfs_sd_fsp_error(context, fsp_result);
    }
    kernel_result = tk_wai_flg(context->transfer_event_flag_id,
        MTFS_SD_EVENT_TRANSFER | MTFS_SD_EVENT_REMOVED,
        TWF_ORW | TWF_BITCLR, &flags,
        (TMO)context->config.transfer_timeout_ms);
    if (kernel_result < E_OK) {
        return mtfs_sd_kernel_error(context, kernel_result);
    }
    if (((flags & MTFS_SD_EVENT_REMOVED) != 0U) ||
        !mtfs_sd_card_present(context)) {
        ++context->diagnostics.media_wait_wakeups;
        if (context->fsp_open != 0U) {
            context->last_fsp_error =
                R_SCI_B_SPI_Close(context->config.spi->p_ctrl);
            context->fsp_open = 0U;
        }
        return mtfs_sd_no_media(context);
    }
    return mtfs_sd_kernel_error(context, context->transfer_result);
}

static mtfs_error_t mtfs_sd_exchange(
    mtfs_ra_sd_spi_context_t *context, uint8_t output, uint8_t *input)
{
    uint8_t received = 0xFFU;
    mtfs_error_t result = mtfs_sd_transfer(context, &output, &received, 1U);
    if ((result == MTFS_OK) && (input != NULL)) {
        *input = received;
    }
    return result;
}

static mtfs_error_t mtfs_sd_end_transaction(mtfs_ra_sd_spi_context_t *context)
{
    mtfs_error_t result = mtfs_sd_cs(context, BSP_IO_LEVEL_HIGH);
    mtfs_error_t clock_result = mtfs_sd_exchange(context, 0xFFU, NULL);
    return (result != MTFS_OK) ? result : clock_result;
}

static uint8_t mtfs_sd_crc7(const uint8_t *data, uint32_t length)
{
    uint8_t crc = 0U;
    uint32_t byte_index;
    unsigned int bit;

    for (byte_index = 0U; byte_index < length; ++byte_index) {
        crc ^= data[byte_index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = ((crc & 0x80U) != 0U) ? (uint8_t)((crc << 1) ^ 0x12U)
                                        : (uint8_t)(crc << 1);
        }
    }
    return (uint8_t)((crc >> 1) & 0x7FU);
}

static mtfs_error_t mtfs_sd_command(
    mtfs_ra_sd_spi_context_t *context, uint8_t command, uint32_t argument, uint8_t *r1)
{
    uint8_t frame[6];
    uint8_t response = 0xFFU;
    unsigned int poll;
    mtfs_error_t result;

    frame[0] = (uint8_t)(0x40U | command);
    frame[1] = (uint8_t)(argument >> 24);
    frame[2] = (uint8_t)(argument >> 16);
    frame[3] = (uint8_t)(argument >> 8);
    frame[4] = (uint8_t)argument;
    frame[5] = (uint8_t)((mtfs_sd_crc7(frame, 5U) << 1) | 1U);

    result = mtfs_sd_exchange(context, 0xFFU, NULL);
    if (result != MTFS_OK) {
        return result;
    }
    result = mtfs_sd_transfer(context, frame, NULL, sizeof(frame));
    if (result != MTFS_OK) {
        return result;
    }
    for (poll = 0U; poll < MTFS_SD_COMMAND_POLLS; ++poll) {
        result = mtfs_sd_exchange(context, 0xFFU, &response);
        if (result != MTFS_OK) {
            return result;
        }
        if ((response & 0x80U) == 0U) {
            context->last_r1 = response;
            *r1 = response;
            return MTFS_OK;
        }
    }
    context->last_r1 = response;
    return MTFS_ERROR_NOT_READY;
}

static mtfs_error_t mtfs_sd_read_bytes(
    mtfs_ra_sd_spi_context_t *context, uint8_t *buffer, uint32_t length)
{
    return mtfs_sd_transfer(context, NULL, buffer, length);
}

static mtfs_error_t mtfs_sd_wait_token(mtfs_ra_sd_spi_context_t *context)
{
    uint8_t token;
    uint32_t polls = 0U;
    int expired;
    mtfs_ra_sd_spi_deadline_t deadline;
    mtfs_error_t result;

    mtfs_sd_diagnostic_increment(&context->diagnostics.token_wait_calls);
    result = mtfs_sd_deadline_start(context,
        context->config.transfer_timeout_ms, &deadline);
    if (result != MTFS_OK) {
        return result;
    }
    for (;;) {
        result = mtfs_sd_deadline_expired(context, &deadline, &expired);
        if (result != MTFS_OK) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.token_max_polls, polls);
            return result;
        }
        if (expired) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.token_max_polls, polls);
            mtfs_sd_diagnostic_increment(&context->diagnostics.token_timeouts);
            return MTFS_ERROR_NOT_READY;
        }
        result = mtfs_sd_exchange(context, 0xFFU, &token);
        ++polls;
        mtfs_sd_diagnostic_increment(&context->diagnostics.token_poll_bytes);
        if (result != MTFS_OK) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.token_max_polls, polls);
            return result;
        }
        if (token == MTFS_SD_DATA_TOKEN) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.token_max_polls, polls);
            return MTFS_OK;
        }
        if (token != 0xFFU) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.token_max_polls, polls);
            return MTFS_ERROR_IO;
        }
    }
}

static mtfs_error_t mtfs_sd_wait_ready(mtfs_ra_sd_spi_context_t *context)
{
    uint8_t value;
    uint32_t polls = 0U;
    int expired;
    mtfs_ra_sd_spi_deadline_t deadline;
    mtfs_error_t result;

    mtfs_sd_diagnostic_increment(&context->diagnostics.ready_wait_calls);
    result = mtfs_sd_deadline_start(context,
        context->config.transfer_timeout_ms, &deadline);
    if (result != MTFS_OK) {
        return result;
    }
    for (;;) {
        result = mtfs_sd_deadline_expired(context, &deadline, &expired);
        if (result != MTFS_OK) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.ready_max_polls, polls);
            return result;
        }
        if (expired) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.ready_max_polls, polls);
            mtfs_sd_diagnostic_increment(&context->diagnostics.ready_timeouts);
            return MTFS_ERROR_NOT_READY;
        }
        result = mtfs_sd_exchange(context, 0xFFU, &value);
        ++polls;
        mtfs_sd_diagnostic_increment(&context->diagnostics.ready_poll_bytes);
        if (result != MTFS_OK) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.ready_max_polls, polls);
            return result;
        }
        if (value == 0xFFU) {
            mtfs_sd_diagnostic_record_polls(
                &context->diagnostics.ready_max_polls, polls);
            return MTFS_OK;
        }
    }
}

static mtfs_error_t mtfs_sd_set_bitrate(
    mtfs_ra_sd_spi_context_t *context, uint32_t bitrate_hz)
{
    fsp_err_t result;

    result = R_SCI_B_SPI_CalculateBitrate(bitrate_hz,
        context->spi_extended_config_ram.clock_source,
        &context->spi_extended_config_ram.clk_div);
    if (result != FSP_SUCCESS) {
        return mtfs_sd_fsp_error(context, result);
    }
    if (context->fsp_open != 0U) {
        result = R_SCI_B_SPI_Close(context->config.spi->p_ctrl);
        if (result != FSP_SUCCESS) {
            return mtfs_sd_fsp_error(context, result);
        }
        context->fsp_open = 0U;
    }
    result = R_SCI_B_SPI_Open(context->config.spi->p_ctrl, &context->spi_config_ram);
    if (result != FSP_SUCCESS) {
        return mtfs_sd_fsp_error(context, result);
    }
    context->fsp_open = 1U;
    result = R_SCI_B_SPI_CallbackSet(
        context->config.spi->p_ctrl, mtfs_ra_sd_spi_callback, context, NULL);
    if (result != FSP_SUCCESS) {
        return mtfs_sd_fsp_error(context, result);
    }
    context->current_bitrate_hz = bitrate_hz;
    return MTFS_OK;
}

static mtfs_error_t mtfs_sd_address(
    const mtfs_ra_sd_spi_context_t *context, mtfs_lba_t lba, uint32_t *argument)
{
    mtfs_lba_t value = lba;
    if (context->card_type == MTFS_RA_SD_CARD_SDSC) {
        if (lba > (UINT32_MAX / MTFS_RA_SD_SPI_SECTOR_SIZE)) {
            return MTFS_ERROR_OUT_OF_RANGE;
        }
        value *= MTFS_RA_SD_SPI_SECTOR_SIZE;
    }
    if (value > UINT32_MAX) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    *argument = (uint32_t)value;
    return MTFS_OK;
}

static mtfs_error_t mtfs_sd_read_register(
    mtfs_ra_sd_spi_context_t *context, uint8_t command, uint8_t *data, uint32_t length)
{
    uint8_t r1;
    uint8_t crc[2];
    mtfs_error_t result;

    result = mtfs_sd_cs(context, BSP_IO_LEVEL_LOW);
    if (result == MTFS_OK) {
        result = mtfs_sd_command(context, command, 0U, &r1);
    }
    if ((result == MTFS_OK) && (r1 != 0U)) {
        result = MTFS_ERROR_IO;
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_wait_token(context);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_read_bytes(context, data, length);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_read_bytes(context, crc, sizeof(crc));
    }
    {
        mtfs_error_t end_result = mtfs_sd_end_transaction(context);
        return (result != MTFS_OK) ? result : end_result;
    }
}

static mtfs_error_t mtfs_sd_decode_capacity(
    const uint8_t csd[16], mtfs_lba_t *sector_count)
{
    uint32_t structure = (uint32_t)((csd[0] >> 6) & 0x03U);
    uint64_t sectors;

    if (structure == 0U) {
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03U) << 10) |
            ((uint32_t)csd[7] << 2) | ((uint32_t)csd[8] >> 6);
        uint32_t multiplier = ((uint32_t)(csd[9] & 0x03U) << 1) |
            ((uint32_t)csd[10] >> 7);
        uint32_t read_block_length = (uint32_t)(csd[5] & 0x0FU);
        uint64_t bytes;
        if (read_block_length > 31U) {
            return MTFS_ERROR_IO;
        }
        bytes = ((uint64_t)c_size + 1U) * (UINT64_C(1) << (multiplier + 2U)) *
            (UINT64_C(1) << read_block_length);
        sectors = bytes / MTFS_RA_SD_SPI_SECTOR_SIZE;
    } else if (structure == 1U) {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3FU) << 16) |
            ((uint32_t)csd[8] << 8) | (uint32_t)csd[9];
        sectors = ((uint64_t)c_size + 1U) * UINT64_C(1024);
    } else {
        return MTFS_ERROR_NOT_SUPPORTED;
    }
    if (sectors == 0U) {
        return MTFS_ERROR_IO;
    }
    *sector_count = (mtfs_lba_t)sectors;
    return MTFS_OK;
}

static mtfs_error_t mtfs_sd_initialize_locked(mtfs_ra_sd_spi_context_t *context)
{
    uint8_t r1 = 0xFFU;
    uint8_t response[4];
    uint8_t csd[16];
    uint32_t index;
    int expired;
    mtfs_ra_sd_spi_deadline_t initialization_deadline;
    int version2 = 0;
    int first_acmd41 = 1;
    mtfs_error_t result;

    mtfs_sd_invalidate_media(context);
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    context->media_removal_pending = 0U;
    result = mtfs_sd_kernel_error(context,
        tk_clr_flg(context->transfer_event_flag_id, 0U));
    if (result != MTFS_OK) {
        return result;
    }
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }

    if (context->device_descriptor <= 0) {
        context->device_descriptor = tk_opn_dev(
            (const UB *)context->config.device_name, TD_UPDATE);
        if (context->device_descriptor <= 0) {
            return mtfs_sd_kernel_error(context, (ER)context->device_descriptor);
        }
    }
    result = mtfs_sd_set_bitrate(context, context->config.initialization_bitrate_hz);
    if (result != MTFS_OK) {
        return result;
    }
    result = mtfs_sd_cs(context, BSP_IO_LEVEL_HIGH);
    if (result != MTFS_OK) {
        return result;
    }
    for (index = 0U; index < 10U; ++index) {
        result = mtfs_sd_exchange(context, 0xFFU, NULL);
        if (result != MTFS_OK) {
            return result;
        }
    }

    result = mtfs_sd_cs(context, BSP_IO_LEVEL_LOW);
    if (result == MTFS_OK) {
        result = mtfs_sd_command(context, MTFS_SD_CMD0, 0U, &r1);
    }
    {
        mtfs_error_t end_result = mtfs_sd_end_transaction(context);
        if (result == MTFS_OK) {
            result = end_result;
        }
    }
    if ((result != MTFS_OK) || (r1 != MTFS_SD_R1_IDLE)) {
        return (result != MTFS_OK) ? result : MTFS_ERROR_NO_MEDIA;
    }

    result = mtfs_sd_cs(context, BSP_IO_LEVEL_LOW);
    if (result == MTFS_OK) {
        result = mtfs_sd_command(context, MTFS_SD_CMD8, UINT32_C(0x1AA), &r1);
    }
    if ((result == MTFS_OK) && (r1 == MTFS_SD_R1_IDLE)) {
        result = mtfs_sd_read_bytes(context, response, sizeof(response));
        if ((result == MTFS_OK) &&
            ((response[2] != 0x01U) || (response[3] != 0xAAU))) {
            result = MTFS_ERROR_NOT_SUPPORTED;
        }
        version2 = 1;
    } else if ((result == MTFS_OK) &&
        ((r1 & (MTFS_SD_R1_IDLE | MTFS_SD_R1_ILLEGAL_COMMAND)) ==
         (MTFS_SD_R1_IDLE | MTFS_SD_R1_ILLEGAL_COMMAND))) {
        version2 = 0;
    } else if (result == MTFS_OK) {
        result = MTFS_ERROR_IO;
    }
    {
        mtfs_error_t end_result = mtfs_sd_end_transaction(context);
        if (result == MTFS_OK) {
            result = end_result;
        }
    }
    if (result != MTFS_OK) {
        return result;
    }

    result = mtfs_sd_deadline_start(context,
        context->config.initialization_timeout_ms, &initialization_deadline);
    if (result != MTFS_OK) {
        return result;
    }
    for (;;) {
        result = mtfs_sd_deadline_expired(context,
            &initialization_deadline, &expired);
        if (result != MTFS_OK) {
            return result;
        }
        if (expired) {
            return MTFS_ERROR_NOT_READY;
        }
        result = mtfs_sd_cs(context, BSP_IO_LEVEL_LOW);
        if (result == MTFS_OK) {
            result = mtfs_sd_command(context, MTFS_SD_CMD55, 0U, &r1);
        }
        if ((result == MTFS_OK) && (r1 > MTFS_SD_R1_IDLE)) {
            result = MTFS_ERROR_IO;
        }
        if (result == MTFS_OK) {
            if (!first_acmd41) {
                mtfs_sd_diagnostic_increment(
                    &context->diagnostics.acmd41_retries);
            }
            first_acmd41 = 0;
            result = mtfs_sd_command(context, MTFS_SD_ACMD41,
                version2 ? UINT32_C(0x40000000) : 0U, &r1);
        }
        {
            mtfs_error_t end_result = mtfs_sd_end_transaction(context);
            if (result == MTFS_OK) {
                result = end_result;
            }
        }
        if (result != MTFS_OK) {
            return result;
        }
        result = mtfs_sd_deadline_expired(context,
            &initialization_deadline, &expired);
        if (result != MTFS_OK) {
            return result;
        }
        if (expired) {
            return MTFS_ERROR_NOT_READY;
        }
        if (r1 == 0U) {
            break;
        }
        if (r1 != MTFS_SD_R1_IDLE) {
            return MTFS_ERROR_IO;
        }
        result = mtfs_sd_kernel_error(context,
            tk_dly_tsk(MTFS_SD_ACMD41_RETRY_DELAY_MS));
        if (result != MTFS_OK) {
            return result;
        }
    }

    result = mtfs_sd_cs(context, BSP_IO_LEVEL_LOW);
    if (result == MTFS_OK) {
        result = mtfs_sd_command(context, MTFS_SD_CMD58, 0U, &r1);
    }
    if ((result == MTFS_OK) && (r1 == 0U)) {
        result = mtfs_sd_read_bytes(context, response, sizeof(response));
    } else if (result == MTFS_OK) {
        result = MTFS_ERROR_IO;
    }
    {
        mtfs_error_t end_result = mtfs_sd_end_transaction(context);
        if (result == MTFS_OK) {
            result = end_result;
        }
    }
    if (result != MTFS_OK) {
        return result;
    }
    context->card_type = (version2 && ((response[0] & 0x40U) != 0U))
        ? MTFS_RA_SD_CARD_SDHC_SDXC : MTFS_RA_SD_CARD_SDSC;

    result = mtfs_sd_read_register(context, MTFS_SD_CMD9, csd, sizeof(csd));
    if (result == MTFS_OK) {
        result = mtfs_sd_decode_capacity(csd, &context->sector_count);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_set_bitrate(context, context->config.data_bitrate_hz);
    }
    if (result == MTFS_OK) {
        context->initialized = 1U;
    }
    return result;
}

static mtfs_error_t mtfs_sd_initialize(void *opaque)
{
    mtfs_ra_sd_spi_context_t *context = (mtfs_ra_sd_spi_context_t *)opaque;
    mtfs_error_t result;
    if (context == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    result = mtfs_sd_lock(context);
    if (result == MTFS_OK) {
        result = mtfs_sd_initialize_locked(context);
        context->last_error = result;
        mtfs_sd_unlock(context);
    }
    return result;
}

static mtfs_error_t mtfs_sd_status(void *opaque, mtfs_block_status_t *status)
{
    mtfs_ra_sd_spi_context_t *context = (mtfs_ra_sd_spi_context_t *)opaque;
    if ((context == NULL) || (status == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    *status = 0U;
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    *status = MTFS_BLOCK_STATUS_MEDIA_PRESENT;
    if ((context->initialized != 0U) &&
        (context->media_removal_pending == 0U)) {
        *status |= MTFS_BLOCK_STATUS_INITIALIZED;
    }
    return mtfs_sd_set_error(context, MTFS_OK);
}

static mtfs_error_t mtfs_sd_read_one(
    mtfs_ra_sd_spi_context_t *context, uint8_t *buffer, mtfs_lba_t lba)
{
    uint8_t r1;
    uint8_t crc[2];
    uint32_t argument;
    mtfs_error_t result = mtfs_sd_address(context, lba, &argument);
    if (result == MTFS_OK) {
        result = mtfs_sd_cs(context, BSP_IO_LEVEL_LOW);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_command(context, MTFS_SD_CMD17, argument, &r1);
    }
    if ((result == MTFS_OK) && (r1 != 0U)) {
        result = MTFS_ERROR_IO;
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_wait_token(context);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_read_bytes(context, buffer, MTFS_RA_SD_SPI_SECTOR_SIZE);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_read_bytes(context, crc, sizeof(crc));
    }
    {
        mtfs_error_t end_result = mtfs_sd_end_transaction(context);
        return (result != MTFS_OK) ? result : end_result;
    }
}

static mtfs_error_t mtfs_sd_write_one(
    mtfs_ra_sd_spi_context_t *context, const uint8_t *buffer, mtfs_lba_t lba)
{
    static const uint8_t crc[2] = {0xFFU, 0xFFU};
    uint8_t r1;
    uint8_t response;
    uint32_t argument;
    mtfs_error_t result = mtfs_sd_address(context, lba, &argument);
    if (result == MTFS_OK) {
        result = mtfs_sd_cs(context, BSP_IO_LEVEL_LOW);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_command(context, MTFS_SD_CMD24, argument, &r1);
    }
    if ((result == MTFS_OK) && (r1 != 0U)) {
        result = MTFS_ERROR_IO;
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_exchange(context, MTFS_SD_DATA_TOKEN, NULL);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_transfer(context, buffer, NULL, MTFS_RA_SD_SPI_SECTOR_SIZE);
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_transfer(context, crc, NULL, sizeof(crc));
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_exchange(context, 0xFFU, &response);
    }
    if ((result == MTFS_OK) &&
        ((response & MTFS_SD_DATA_RESPONSE_MASK) != MTFS_SD_DATA_ACCEPTED)) {
        result = MTFS_ERROR_IO;
    }
    if (result == MTFS_OK) {
        result = mtfs_sd_wait_ready(context);
    }
    {
        mtfs_error_t end_result = mtfs_sd_end_transaction(context);
        return (result != MTFS_OK) ? result : end_result;
    }
}

static mtfs_error_t mtfs_sd_read(void *opaque, void *buffer, mtfs_lba_t lba, uint32_t count)
{
    mtfs_ra_sd_spi_context_t *context = (mtfs_ra_sd_spi_context_t *)opaque;
    uint8_t *cursor = (uint8_t *)buffer;
    uint32_t index;
    mtfs_error_t result;
    if ((context == NULL) || (buffer == NULL) || (count == 0U)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    if ((context->initialized == 0U) || context->media_removal_pending) {
        return mtfs_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    result = mtfs_sd_lock(context);
    if (result != MTFS_OK) {
        return result;
    }
    for (index = 0U; (result == MTFS_OK) && (index < count); ++index) {
        if (!mtfs_sd_card_present(context)) {
            result = mtfs_sd_no_media(context);
            break;
        }
        result = mtfs_sd_read_one(context, cursor, lba + index);
        if (result == MTFS_OK) {
            ++context->diagnostics.read_sectors;
            cursor += MTFS_RA_SD_SPI_SECTOR_SIZE;
        }
    }
    context->last_error = result;
    mtfs_sd_unlock(context);
    return result;
}

static mtfs_error_t mtfs_sd_write(void *opaque, const void *buffer, mtfs_lba_t lba, uint32_t count)
{
    mtfs_ra_sd_spi_context_t *context = (mtfs_ra_sd_spi_context_t *)opaque;
    const uint8_t *cursor = (const uint8_t *)buffer;
    uint32_t index;
    mtfs_error_t result;
    if ((context == NULL) || (buffer == NULL) || (count == 0U)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    if ((context->initialized == 0U) || context->media_removal_pending) {
        return mtfs_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    result = mtfs_sd_lock(context);
    if (result != MTFS_OK) {
        return result;
    }
    for (index = 0U; (result == MTFS_OK) && (index < count); ++index) {
        if (!mtfs_sd_card_present(context)) {
            result = mtfs_sd_no_media(context);
            break;
        }
        result = mtfs_sd_write_one(context, cursor, lba + index);
        if (result == MTFS_OK) {
            ++context->diagnostics.write_sectors;
            cursor += MTFS_RA_SD_SPI_SECTOR_SIZE;
        }
    }
    context->last_error = result;
    mtfs_sd_unlock(context);
    return result;
}

static mtfs_error_t mtfs_sd_sync(void *opaque)
{
    mtfs_ra_sd_spi_context_t *context = (mtfs_ra_sd_spi_context_t *)opaque;
    if (context == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    return ((context->initialized != 0U) &&
        !context->media_removal_pending)
        ? mtfs_sd_set_error(context, MTFS_OK)
        : mtfs_sd_set_error(context, MTFS_ERROR_NOT_READY);
}

static mtfs_error_t mtfs_sd_geometry(void *opaque, mtfs_block_geometry_t *geometry)
{
    mtfs_ra_sd_spi_context_t *context = (mtfs_ra_sd_spi_context_t *)opaque;
    if ((context == NULL) || (geometry == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!mtfs_sd_card_present(context)) {
        return mtfs_sd_no_media(context);
    }
    if ((context->initialized == 0U) || context->media_removal_pending ||
        (context->sector_count == 0U)) {
        return mtfs_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    geometry->sector_size = MTFS_RA_SD_SPI_SECTOR_SIZE;
    geometry->sector_count = context->sector_count;
    geometry->erase_block_size = MTFS_SD_ERASE_BLOCK_SECTORS;
    return mtfs_sd_set_error(context, MTFS_OK);
}

static mtfs_error_t mtfs_sd_trim(void *opaque, mtfs_lba_t lba, mtfs_lba_t count)
{
    (void)opaque;
    (void)lba;
    (void)count;
    return MTFS_ERROR_NOT_SUPPORTED;
}

mtfs_error_t mtfs_ra_sd_spi_context_init(
    mtfs_ra_sd_spi_context_t *context,
    const mtfs_ra_sd_spi_config_t *config)
{
    T_CFLG event_flag = {
        .exinf = NULL,
        .flgatr = TA_TFIFO | TA_WMUL,
        .iflgptn = 0U
    };
    T_CMTX mutex = {
        .exinf = NULL,
        .mtxatr = TA_INHERIT,
        .ceilpri = 0
    };
    T_DDEV driver;
    T_IDEV initial_device;
    ID result;
    uint32_t index;

    if ((context == NULL) || (config == NULL) || (config->device_name == NULL) ||
        (config->spi == NULL) || (config->spi->p_ctrl == NULL) ||
        (config->spi->p_cfg == NULL) || (config->spi->p_cfg->p_extend == NULL) ||
        (config->ioport == NULL) || (config->ioport->p_api == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    memset(context, 0, sizeof(*context));
    context->config = *config;
    if (context->config.initialization_bitrate_hz == 0U) {
        context->config.initialization_bitrate_hz = MTFS_RA_SD_SPI_DEFAULT_INIT_HZ;
    }
    if (context->config.data_bitrate_hz == 0U) {
        context->config.data_bitrate_hz = MTFS_RA_SD_SPI_DEFAULT_DATA_HZ;
    }
    if (context->config.initialization_timeout_ms == 0U) {
        context->config.initialization_timeout_ms = MTFS_RA_SD_SPI_DEFAULT_TIMEOUT_MS;
    }
    if (context->config.transfer_timeout_ms == 0U) {
        context->config.transfer_timeout_ms = MTFS_RA_SD_SPI_DEFAULT_TIMEOUT_MS;
    }
    if (context->config.initialization_bitrate_hz > 400000U) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    context->device_descriptor = (ID)-1;
    context->device_id = (ID)-1;
    context->last_fsp_error = FSP_SUCCESS;
    context->last_kernel_error = E_OK;
    context->last_error = MTFS_OK;
    context->spi_config_ram = *config->spi->p_cfg;
    context->spi_extended_config_ram =
        *(const sci_b_spi_extended_cfg_t *)config->spi->p_cfg->p_extend;
    context->spi_config_ram.p_extend = &context->spi_extended_config_ram;
    for (index = 0U; index < MTFS_RA_SD_SPI_DUMMY_SIZE; ++index) {
        context->dummy_tx[index] = 0xFFU;
    }

    result = tk_cre_flg(&event_flag);
    if (result <= 0) {
        return mtfs_sd_kernel_error(context, (ER)result);
    }
    context->transfer_event_flag_id = result;
    result = tk_cre_mtx(&mutex);
    if (result <= 0) {
        (void)tk_del_flg(context->transfer_event_flag_id);
        context->transfer_event_flag_id = 0;
        return mtfs_sd_kernel_error(context, (ER)result);
    }
    context->access_mutex_id = result;

    memset(&driver, 0, sizeof(driver));
    driver.exinf = context;
    driver.devatr = TDK_UNDEF;
    driver.nsub = 0;
    driver.blksz = 1;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
    driver.openfn = (FP)mtfs_sd_driver_open;
    driver.closefn = (FP)mtfs_sd_driver_close;
    driver.execfn = (FP)mtfs_sd_driver_execute;
    driver.waitfn = (FP)mtfs_sd_driver_wait;
    driver.abortfn = (FP)mtfs_sd_driver_abort;
    driver.eventfn = (FP)mtfs_sd_driver_event;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    result = tk_def_dev((const UB *)context->config.device_name, &driver, &initial_device);
    if (result <= 0) {
        (void)tk_del_mtx(context->access_mutex_id);
        (void)tk_del_flg(context->transfer_event_flag_id);
        context->access_mutex_id = 0;
        context->transfer_event_flag_id = 0;
        return mtfs_sd_kernel_error(context, (ER)result);
    }
    context->device_id = result;
    context->registered = 1U;
    context->block_device.ops = &mtfs_sd_ops;
    context->block_device.context = context;
    context->block_device.capabilities = 0U;
    return MTFS_OK;
}

mtfs_error_t mtfs_ra_sd_spi_context_deinit(mtfs_ra_sd_spi_context_t *context)
{
    mtfs_error_t final_result = MTFS_OK;
    ER result;
    if (context == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    context->media_removal_pending = 1U;
    mtfs_sd_invalidate_media(context);
    if (context->device_descriptor > 0) {
        result = tk_cls_dev(context->device_descriptor, 0U);
        if (result < E_OK) {
            final_result = mtfs_sd_kernel_error(context, result);
        }
        context->device_descriptor = (ID)-1;
    }
    if (context->registered != 0U) {
        result = tk_def_dev((const UB *)context->config.device_name, NULL, NULL);
        if ((result < E_OK) && (final_result == MTFS_OK)) {
            final_result = mtfs_sd_kernel_error(context, result);
        }
        context->registered = 0U;
    }
    if (context->access_mutex_id > 0) {
        result = tk_del_mtx(context->access_mutex_id);
        if ((result < E_OK) && (final_result == MTFS_OK)) {
            final_result = mtfs_sd_kernel_error(context, result);
        }
        context->access_mutex_id = 0;
    }
    if (context->transfer_event_flag_id > 0) {
        result = tk_del_flg(context->transfer_event_flag_id);
        if ((result < E_OK) && (final_result == MTFS_OK)) {
            final_result = mtfs_sd_kernel_error(context, result);
        }
        context->transfer_event_flag_id = 0;
    }
    context->last_error = final_result;
    return final_result;
}

mtfs_block_device_t *mtfs_ra_sd_spi_block_device(mtfs_ra_sd_spi_context_t *context)
{
    return (context == NULL) ? NULL : &context->block_device;
}

mtfs_error_t mtfs_ra_sd_spi_media_changed_isr(
    mtfs_ra_sd_spi_context_t *context, int present)
{
    ER result;

    if ((context == NULL) || (context->transfer_event_flag_id <= 0) ||
        (context->config.card_present == NULL)) {
        return MTFS_ERROR_NOT_READY;
    }
    if (present) {
        return MTFS_OK;
    }

    context->media_removal_pending = 1U;
    context->initialized = 0U;
    ++context->diagnostics.media_removal_notifications;
    result = tk_set_flg(context->transfer_event_flag_id,
        MTFS_SD_EVENT_REMOVED);
    if (result < E_OK) {
        context->last_kernel_error = result;
        return MTFS_ERROR_IO;
    }
    return MTFS_OK;
}
