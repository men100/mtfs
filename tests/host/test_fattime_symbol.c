#include <stdint.h>

#include "ff.h"
#include "mtfs_time.h"

typedef struct fake_context
{
    mtfs_datetime_t datetime;
    mtfs_time_status_t status;
    int fail;
} fake_context_t;

static mtfs_error_t fake_get(
    void *opaque, mtfs_datetime_t *datetime, mtfs_time_status_t *status)
{
    fake_context_t *context = (fake_context_t *)opaque;
    if (context->fail) {
        return MTFS_ERROR_IO;
    }
    *datetime = context->datetime;
    *status = context->status;
    return MTFS_OK;
}

static mtfs_error_t fake_set(void *opaque, const mtfs_datetime_t *datetime)
{
    fake_context_t *context = (fake_context_t *)opaque;
    context->datetime = *datetime;
    return MTFS_OK;
}

static mtfs_error_t fake_status(void *opaque, mtfs_time_status_t *status)
{
    *status = ((fake_context_t *)opaque)->status;
    return MTFS_OK;
}

static mtfs_error_t fake_clear(void *opaque)
{
    ((fake_context_t *)opaque)->status = MTFS_TIME_STATUS_UNSET;
    return MTFS_OK;
}

int main(void)
{
    static const mtfs_time_provider_ops_t ops = {
        fake_get, fake_set, fake_status, fake_clear
    };
    fake_context_t context = {
        {2026U, 8U, 14U, 21U, 30U, 3U}, MTFS_TIME_STATUS_VALID, 0
    };
    mtfs_time_provider_t provider = {&ops, &context, 0, 0, 0};
    DWORD expected = ((DWORD)(2026U - 1980U) << 25) |
        ((DWORD)8U << 21) | ((DWORD)14U << 16) |
        ((DWORD)21U << 11) | ((DWORD)30U << 5) | 1U;

    if ((get_fattime() != 0U) ||
        (mtfs_time_provider_register(&provider) != MTFS_OK) ||
        (get_fattime() != expected)) {
        return 1;
    }
    context.status = MTFS_TIME_STATUS_UNSET;
    if (get_fattime() != 0U) {
        return 1;
    }
    context.status = MTFS_TIME_STATUS_ERROR;
    if (get_fattime() != 0U) {
        return 1;
    }
    context.status = MTFS_TIME_STATUS_UNAVAILABLE;
    if (get_fattime() != 0U) {
        return 1;
    }
    context.status = MTFS_TIME_STATUS_VALID;
    context.fail = 1;
    return get_fattime() == 0U ? 0 : 1;
}
