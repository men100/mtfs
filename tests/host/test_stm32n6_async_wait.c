#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mtfs_stm32n6_async_wait.h"
#include "mtfs_stm32n6_aton_osal.h"
#include <tk/tkernel.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return 1; } } while (0)

static int fake_flag_created;
static UINT fake_flag_pattern;
static ER fake_wait_error;
static int fake_notify_during_wait;
static uint32_t fake_set_calls;
static uint32_t fake_irq_priority;

ID tk_cre_flg(const T_CFLG *config)
{
    (void)config;
    fake_flag_created = 1;
    fake_flag_pattern = 0U;
    return 1;
}

ER tk_del_flg(ID flag_id)
{
    if (flag_id != 1 || !fake_flag_created) return E_NOEXS;
    fake_flag_created = 0;
    fake_flag_pattern = 0U;
    return E_OK;
}

ER tk_set_flg(ID flag_id, UINT pattern)
{
    if (flag_id != 1 || !fake_flag_created) return E_NOEXS;
    fake_flag_pattern |= pattern;
    ++fake_set_calls;
    return E_OK;
}

ER tk_wai_flg(ID flag_id, UINT wait_pattern, UINT wait_mode,
    UINT *matched_pattern, TMO timeout)
{
    (void)wait_mode;
    if (flag_id != 1 || !fake_flag_created) return E_NOEXS;
    if (fake_wait_error != E_OK) return fake_wait_error;
    if ((fake_flag_pattern & wait_pattern) == 0U && timeout != TMO_POL &&
        fake_notify_during_wait) {
        fake_notify_during_wait = 0;
        mtfs_stm32n6_aton_osal_signal_event();
    }
    if ((fake_flag_pattern & wait_pattern) == 0U) return E_TMOUT;
    *matched_pattern = fake_flag_pattern;
    fake_flag_pattern &= ~wait_pattern;
    return E_OK;
}

void NVIC_SetPriority(int32_t interrupt, uint32_t priority)
{
    (void)interrupt;
    fake_irq_priority = priority;
}

typedef struct fake_async
{
    mtfs_stm32n6_async_state_t states[20];
    mtfs_stm32n6_event_wait_result_t waits[20];
    uint32_t wait_advance[20];
    uint8_t immediate[20];
    uint32_t state_count;
    uint32_t state_index;
    uint32_t wait_count;
    uint32_t wait_index;
    uint32_t now;
    uint32_t last_remaining;
    uint32_t recovery_count;
} fake_async_t;

static mtfs_stm32n6_async_state_t fake_run_epoch(void *opaque)
{
    fake_async_t *fake = opaque;
    if (fake->state_index >= fake->state_count)
        return MTFS_STM32N6_ASYNC_UNKNOWN;
    return fake->states[fake->state_index++];
}

static uint32_t fake_clock_ms(void *opaque)
{
    return ((fake_async_t *)opaque)->now;
}

static mtfs_stm32n6_event_wait_result_t fake_wait_event(void *opaque,
    uint32_t remaining, int *immediate)
{
    fake_async_t *fake = opaque;
    uint32_t index = fake->wait_index++;
    fake->last_remaining = remaining;
    if (index >= fake->wait_count)
        return MTFS_STM32N6_EVENT_KERNEL_ERROR;
    fake->now += fake->wait_advance[index];
    *immediate = fake->immediate[index] != 0U;
    return fake->waits[index];
}

static void fake_recover(void *opaque)
{
    ++((fake_async_t *)opaque)->recovery_count;
}

static mtfs_stm32n6_async_config_t fake_config(fake_async_t *fake,
    uint32_t timeout, uint32_t no_wfe_limit)
{
    mtfs_stm32n6_async_config_t config;
    (void)memset(&config, 0, sizeof(config));
    config.context = fake;
    config.run_epoch = fake_run_epoch;
    config.clock_ms = fake_clock_ms;
    config.wait_event = fake_wait_event;
    config.recover = fake_recover;
    config.timeout_ms = timeout;
    config.consecutive_no_wfe_limit = no_wfe_limit;
    return config;
}

static int test_state_runner(void)
{
    fake_async_t fake;
    mtfs_stm32n6_async_config_t config;
    mtfs_stm32n6_async_diagnostics_t diagnostics;

    (void)memset(&fake, 0, sizeof(fake));
    fake.states[0] = MTFS_STM32N6_ASYNC_NO_WFE;
    fake.states[1] = MTFS_STM32N6_ASYNC_WFE;
    fake.states[2] = MTFS_STM32N6_ASYNC_WFE;
    fake.states[3] = MTFS_STM32N6_ASYNC_DONE;
    fake.state_count = 4U;
    fake.waits[0] = MTFS_STM32N6_EVENT_NOTIFIED;
    fake.waits[1] = MTFS_STM32N6_EVENT_NOTIFIED;
    fake.wait_count = 2U;
    config = fake_config(&fake, 100U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) == MTFS_OK);
    CHECK(fake.state_index == 4U && fake.wait_index == 2U);
    CHECK(diagnostics.state_no_wfe == 1U &&
        diagnostics.state_wfe == 2U && diagnostics.state_done == 1U);
    CHECK(diagnostics.event_wait_starts == 2U &&
        diagnostics.completed == 1U && diagnostics.failed == 0U);

    (void)memset(&fake, 0, sizeof(fake));
    fake.states[0] = MTFS_STM32N6_ASYNC_WFE;
    fake.states[1] = MTFS_STM32N6_ASYNC_DONE;
    fake.state_count = 2U;
    fake.waits[0] = MTFS_STM32N6_EVENT_NOTIFIED;
    fake.immediate[0] = 1U;
    fake.wait_count = 1U;
    config = fake_config(&fake, 100U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) == MTFS_OK);
    CHECK(diagnostics.immediate_event_completions == 1U);

    (void)memset(&fake, 0, sizeof(fake));
    fake.states[0] = MTFS_STM32N6_ASYNC_WFE;
    fake.states[1] = MTFS_STM32N6_ASYNC_DONE;
    fake.state_count = 2U;
    fake.waits[0] = MTFS_STM32N6_EVENT_SPURIOUS;
    fake.waits[1] = MTFS_STM32N6_EVENT_LATE;
    fake.waits[2] = MTFS_STM32N6_EVENT_NOTIFIED;
    fake.wait_count = 3U;
    config = fake_config(&fake, 100U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) == MTFS_OK);
    CHECK(diagnostics.spurious_notifications == 1U &&
        diagnostics.late_notifications == 1U &&
        diagnostics.event_wait_starts == 3U);

    (void)memset(&fake, 0, sizeof(fake));
    fake.states[0] = MTFS_STM32N6_ASYNC_WFE;
    fake.state_count = 1U;
    fake.waits[0] = MTFS_STM32N6_EVENT_TIMEOUT;
    fake.wait_advance[0] = 25U;
    fake.wait_count = 1U;
    config = fake_config(&fake, 25U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) ==
        MTFS_ERROR_NOT_READY);
    CHECK(diagnostics.timeouts == 1U && diagnostics.recoveries == 1U &&
        fake.last_remaining == 25U);

    (void)memset(&fake, 0, sizeof(fake));
    fake.now = UINT32_MAX - 2U;
    fake.states[0] = MTFS_STM32N6_ASYNC_WFE;
    fake.states[1] = MTFS_STM32N6_ASYNC_DONE;
    fake.state_count = 2U;
    fake.waits[0] = MTFS_STM32N6_EVENT_NOTIFIED;
    fake.wait_advance[0] = 5U;
    fake.wait_count = 1U;
    config = fake_config(&fake, 10U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) == MTFS_OK);

    (void)memset(&fake, 0, sizeof(fake));
    fake.states[0] = MTFS_STM32N6_ASYNC_WFE;
    fake.state_count = 1U;
    fake.waits[0] = MTFS_STM32N6_EVENT_KERNEL_ERROR;
    fake.wait_count = 1U;
    config = fake_config(&fake, 100U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) == MTFS_ERROR_IO);
    CHECK(diagnostics.kernel_wait_errors == 1U &&
        diagnostics.recoveries == 1U);

    (void)memset(&fake, 0, sizeof(fake));
    fake.states[0] = MTFS_STM32N6_ASYNC_UNKNOWN;
    fake.state_count = 1U;
    config = fake_config(&fake, 100U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) == MTFS_ERROR_IO);
    CHECK(diagnostics.state_other == 1U && diagnostics.recoveries == 1U);

    (void)memset(&fake, 0, sizeof(fake));
    fake.states[0] = MTFS_STM32N6_ASYNC_NO_WFE;
    fake.states[1] = MTFS_STM32N6_ASYNC_NO_WFE;
    fake.states[2] = MTFS_STM32N6_ASYNC_NO_WFE;
    fake.state_count = 3U;
    config = fake_config(&fake, 100U, 3U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) ==
        MTFS_ERROR_NOT_READY);
    CHECK(diagnostics.consecutive_no_wfe_max == 3U &&
        diagnostics.consecutive_no_wfe_limit_errors == 1U);

    (void)memset(&fake, 0, sizeof(fake));
    fake.states[0] = MTFS_STM32N6_ASYNC_WFE;
    fake.state_count = 1U;
    fake.waits[0] = MTFS_STM32N6_EVENT_TIMEOUT;
    fake.wait_advance[0] = 10U;
    fake.wait_count = 1U;
    config = fake_config(&fake, 10U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) ==
        MTFS_ERROR_NOT_READY);
    CHECK(fake.recovery_count == 1U);
    (void)memset(fake.states, 0, sizeof(fake.states));
    (void)memset(fake.waits, 0, sizeof(fake.waits));
    fake.state_index = 0U;
    fake.wait_index = 0U;
    fake.state_count = 1U;
    fake.states[0] = MTFS_STM32N6_ASYNC_DONE;
    fake.wait_count = 0U;
    config = fake_config(&fake, 10U, 8U);
    CHECK(mtfs_stm32n6_async_run(&config, &diagnostics) == MTFS_OK);
    CHECK(diagnostics.completed == 1U && fake.recovery_count == 1U);
    return 0;
}

static void reset_fake_kernel(void)
{
    fake_flag_created = 0;
    fake_flag_pattern = 0U;
    fake_wait_error = E_OK;
    fake_notify_during_wait = 0;
    fake_set_calls = 0U;
    fake_irq_priority = 0U;
}

static int test_osal_event_contract(void)
{
    mtfs_stm32n6_aton_osal_diagnostics_t diagnostics;
    int immediate;
    reset_fake_kernel();
    mtfs_stm32n6_aton_osal_init();
    CHECK(mtfs_stm32n6_aton_osal_ready());
    CHECK(fake_irq_priority == 6U);
    CHECK(mtfs_stm32n6_aton_osal_diagnostics_reset() == MTFS_OK);
    CHECK(mtfs_stm32n6_aton_osal_inference_begin() == MTFS_OK);
    CHECK(mtfs_stm32n6_aton_osal_diagnostics_reset() ==
        MTFS_ERROR_INVALID_STATE);

    mtfs_stm32n6_aton_osal_signal_event();
    CHECK(mtfs_stm32n6_aton_osal_wait_event(100U, &immediate) ==
        MTFS_STM32N6_EVENT_NOTIFIED && immediate == 1);
    fake_notify_during_wait = 1;
    CHECK(mtfs_stm32n6_aton_osal_wait_event(100U, &immediate) ==
        MTFS_STM32N6_EVENT_NOTIFIED && immediate == 0);

    mtfs_stm32n6_aton_osal_signal_event();
    mtfs_stm32n6_aton_osal_signal_event();
    CHECK(mtfs_stm32n6_aton_osal_wait_event(100U, &immediate) ==
        MTFS_STM32N6_EVENT_NOTIFIED && immediate == 1);
    mtfs_stm32n6_aton_osal_inference_end();
    mtfs_stm32n6_aton_osal_diagnostics_get(&diagnostics);
    CHECK(diagnostics.irq_notifications == 4U);
    CHECK(diagnostics.consumed_notifications == 3U);
    CHECK(diagnostics.spurious_notifications == 1U);
    CHECK(diagnostics.late_notifications == 0U);

    mtfs_stm32n6_aton_osal_signal_event();
    mtfs_stm32n6_aton_osal_diagnostics_get(&diagnostics);
    CHECK(diagnostics.late_notifications == 1U);
    mtfs_stm32n6_aton_osal_deinit();
    mtfs_stm32n6_aton_osal_signal_event();
    mtfs_stm32n6_aton_osal_diagnostics_get(&diagnostics);
    CHECK(diagnostics.late_notifications == 2U);
    CHECK(fake_set_calls == 4U);
    CHECK(!mtfs_stm32n6_aton_osal_ready());

    mtfs_stm32n6_aton_osal_init();
    CHECK(mtfs_stm32n6_aton_osal_diagnostics_reset() == MTFS_OK);
    CHECK(mtfs_stm32n6_aton_osal_inference_begin() == MTFS_OK);
    fake_wait_error = E_OBJ;
    CHECK(mtfs_stm32n6_aton_osal_wait_event(100U, &immediate) ==
        MTFS_STM32N6_EVENT_KERNEL_ERROR);
    fake_wait_error = E_OK;
    mtfs_stm32n6_aton_osal_inference_end();
    mtfs_stm32n6_aton_osal_deinit();
    return 0;
}

int main(void)
{
    CHECK(test_state_runner() == 0);
    CHECK(test_osal_event_contract() == 0);
    puts("STM32N6 async wait tests passed");
    return 0;
}
