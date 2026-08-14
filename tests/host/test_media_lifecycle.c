#include "test_media_lifecycle.h"

#include <string.h>

#include "mtfs_media.h"

typedef struct fake_media
{
    int raw_level;
    int read_error;
    int in_isr;
    unsigned int inserted;
    unsigned int removed;
    unsigned int errors;
    unsigned int callback_in_isr;
} fake_media_t;

static int fake_read_signal(void *opaque)
{
    fake_media_t *fake = opaque;
    return fake->read_error ? -1 : fake->raw_level;
}

static void fake_media_event(void *opaque, mtfs_media_event_t event,
    mtfs_media_state_t state)
{
    fake_media_t *fake = opaque;
    (void)state;
    if (fake->in_isr) {
        ++fake->callback_in_isr;
    }
    if (event == MTFS_MEDIA_EVENT_INSERTED) {
        ++fake->inserted;
    } else if (event == MTFS_MEDIA_EVENT_REMOVED) {
        ++fake->removed;
    } else {
        ++fake->errors;
    }
}

static mtfs_error_t fake_irq(mtfs_media_context_t *media,
    fake_media_t *fake, int raw_level)
{
    mtfs_error_t result;
    fake->raw_level = raw_level;
    fake->in_isr = 1;
    result = mtfs_media_notify_isr(media, raw_level);
    fake->in_isr = 0;
    return result;
}

static mtfs_media_config_t fake_config(fake_media_t *fake)
{
    mtfs_media_config_t config;
    (void)memset(&config, 0, sizeof(config));
    config.read_signal = fake_read_signal;
    config.signal_context = fake;
    config.event_callback = fake_media_event;
    config.event_context = fake;
    config.debounce_ms = 25U;
    config.active_level = MTFS_MEDIA_ACTIVE_HIGH;
    return config;
}

int test_media_lifecycle(mtfs_test_t *test)
{
    mtfs_media_context_t media;
    fake_media_t fake;
    mtfs_media_config_t config;
    uint32_t wait_ms;

    (void)memset(&fake, 0, sizeof(fake));
    config = fake_config(&fake);
    if (!MTFS_TEST_CHECK(test,
            mtfs_media_init(&media, &config) == MTFS_OK &&
            mtfs_media_state(&media) == MTFS_MEDIA_STATE_ABSENT &&
            fake.inserted == 0U && fake.removed == 0U,
            "media starts ABSENT without synthesizing an edge")) {
        return 1;
    }

    if (!MTFS_TEST_CHECK(test,
            fake_irq(&media, &fake, 1) == MTFS_OK &&
            fake.callback_in_isr == 0U && fake.inserted == 0U,
            "ISR notification only records state")) {
        return 1;
    }
    (void)mtfs_media_process(&media, 0U, &wait_ms);
    if (!MTFS_TEST_CHECK(test,
            wait_ms == 25U &&
            mtfs_media_state(&media) ==
                MTFS_MEDIA_STATE_DEBOUNCING_INSERT,
            "insert edge begins a one-shot debounce")) {
        return 1;
    }
    (void)mtfs_media_process(&media, 24U, &wait_ms);
    (void)mtfs_media_process(&media, 25U, &wait_ms);
    if (!MTFS_TEST_CHECK(test,
            mtfs_media_state(&media) == MTFS_MEDIA_STATE_PRESENT &&
            fake.inserted == 1U && wait_ms == MTFS_MEDIA_WAIT_FOREVER,
            "stable insertion emits INSERTED from task processing")) {
        return 1;
    }

    (void)fake_irq(&media, &fake, 1);
    (void)mtfs_media_process(&media, 30U, &wait_ms);
    (void)mtfs_media_process(&media, 55U, &wait_ms);
    if (!MTFS_TEST_CHECK(test, fake.inserted == 1U,
            "same stable state does not emit a duplicate event")) {
        return 1;
    }

    (void)fake_irq(&media, &fake, 0);
    (void)mtfs_media_process(&media, 100U, &wait_ms);
    (void)fake_irq(&media, &fake, 1);
    (void)mtfs_media_process(&media, 105U, &wait_ms);
    (void)fake_irq(&media, &fake, 0);
    (void)mtfs_media_process(&media, 110U, &wait_ms);
    (void)mtfs_media_process(&media, 135U, &wait_ms);
    if (!MTFS_TEST_CHECK(test,
            fake.removed == 1U &&
            mtfs_media_state(&media) == MTFS_MEDIA_STATE_ABSENT,
            "bounce edges converge on the latest GPIO level")) {
        return 1;
    }

    (void)fake_irq(&media, &fake, 1);
    (void)mtfs_media_process(&media, 200U, &wait_ms);
    fake.raw_level = 0;
    (void)mtfs_media_process(&media, 225U, &wait_ms);
    (void)mtfs_media_process(&media, 250U, &wait_ms);
    if (!MTFS_TEST_CHECK(test,
            fake.inserted == 1U && fake.removed == 1U,
            "state inversion during debounce is rechecked without an event")) {
        return 1;
    }

    (void)fake_irq(&media, &fake, 1);
    (void)fake_irq(&media, &fake, 0);
    (void)fake_irq(&media, &fake, 1);
    (void)mtfs_media_process(&media, 300U, &wait_ms);
    (void)mtfs_media_process(&media, 325U, &wait_ms);
    if (!MTFS_TEST_CHECK(test, fake.inserted == 2U,
            "coalesced notifications settle from a fresh GPIO read")) {
        return 1;
    }

    fake.raw_level = 0;
    (void)mtfs_media_notify(&media);
    (void)mtfs_media_process(&media, 400U, &wait_ms);
    (void)mtfs_media_process(&media, 425U, &wait_ms);
    if (!MTFS_TEST_CHECK(test,
            fake.removed == 2U && media.diagnostics.manual_notifications == 1U,
            "manual notification uses the same debounce path")) {
        return 1;
    }

    fake.raw_level = 1;
    (void)mtfs_media_poll(&media, 500U, &wait_ms);
    (void)mtfs_media_poll(&media, 525U, &wait_ms);
    if (!MTFS_TEST_CHECK(test,
            fake.inserted == 3U && media.diagnostics.poll_checks == 2U,
            "explicit poll fallback detects a state change")) {
        return 1;
    }

    if (!MTFS_TEST_CHECK(test,
            mtfs_media_stop_notifications(&media) == MTFS_OK &&
            fake_irq(&media, &fake, 0) == MTFS_ERROR_NOT_READY &&
            mtfs_media_deinit(&media) == MTFS_OK &&
            mtfs_media_notify_isr(&media, 0) == MTFS_ERROR_NOT_READY,
            "shutdown rejects notifications before and after deinit")) {
        return 1;
    }

    (void)memset(&fake, 0, sizeof(fake));
    fake.raw_level = 1;
    config = fake_config(&fake);
    if (!MTFS_TEST_CHECK(test,
            mtfs_media_init(&media, &config) == MTFS_OK &&
            mtfs_media_state(&media) == MTFS_MEDIA_STATE_PRESENT &&
            fake.inserted == 0U,
            "media can reinitialize PRESENT without an edge event")) {
        return 1;
    }
    fake.read_error = 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_media_notify(&media) == MTFS_ERROR_IO &&
            fake.errors == 1U && fake.callback_in_isr == 0U,
            "signal errors notify once from task context")) {
        return 1;
    }
    return 0;
}
