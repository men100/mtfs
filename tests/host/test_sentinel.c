#include "test_sentinel.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "mtfs_sentinel.h"

typedef struct sentinel_fake {
    pthread_mutex_t mutex;
    pthread_mutex_t downstream_mutex;
    pthread_mutex_t clock_mutex;
    pthread_cond_t downstream_condition;
    mtfs_error_t result;
    uint64_t now;
    uint64_t latency;
    uint32_t clock_phase;
    uint32_t clock_fail;
    uint32_t calls[7];
    uint32_t blocked_operation;
    uint32_t operation_entered;
    uint32_t release_operation;
    uint64_t lock_calls;
    uint64_t unlock_calls;
} sentinel_fake_t;

static mtfs_error_t fake_lock(void *p)
{
    sentinel_fake_t *f = p;
    if (pthread_mutex_lock(&f->mutex) != 0) return MTFS_ERROR_INVALID_STATE;
    ++f->lock_calls;
    return MTFS_OK;
}
static void fake_unlock(void *p)
{
    sentinel_fake_t *f = p;
    ++f->unlock_calls;
    (void)pthread_mutex_unlock(&f->mutex);
}
static mtfs_error_t fake_clock(void *p, uint64_t *now_us)
{
    sentinel_fake_t *f = p;
    (void)pthread_mutex_lock(&f->clock_mutex);
    if (f->clock_fail != 0U) {
        --f->clock_fail;
        (void)pthread_mutex_unlock(&f->clock_mutex);
        return MTFS_ERROR_NOT_READY;
    }
    *now_us = f->now;
    if ((f->clock_phase++ & 1U) == 0U) f->now += f->latency;
    (void)pthread_mutex_unlock(&f->clock_mutex);
    return MTFS_OK;
}
static mtfs_error_t fake_call(void *p, uint32_t operation)
{
    sentinel_fake_t *f=p; mtfs_error_t result;
    (void)pthread_mutex_lock(&f->downstream_mutex);
    ++f->calls[operation];
    if (operation == f->blocked_operation) {
        f->operation_entered = 1U;
        (void)pthread_cond_broadcast(&f->downstream_condition);
        while (f->release_operation == 0U)
            (void)pthread_cond_wait(&f->downstream_condition,
                &f->downstream_mutex);
    }
    result=f->result;
    (void)pthread_mutex_unlock(&f->downstream_mutex); return result;
}
static mtfs_error_t f_init(void *p) { return fake_call(p,0U); }
static mtfs_error_t f_status(void *p, mtfs_block_status_t *s) { *s=MTFS_BLOCK_STATUS_MEDIA_PRESENT;return fake_call(p,1U); }
static mtfs_error_t f_read(void *p, void *b, mtfs_lba_t l, uint32_t c) { (void)b;(void)l;(void)c;return fake_call(p,2U); }
static mtfs_error_t f_write(void *p, const void *b, mtfs_lba_t l, uint32_t c) { (void)b;(void)l;(void)c;return fake_call(p,3U); }
static mtfs_error_t f_sync(void *p) { return fake_call(p,4U); }
static mtfs_error_t f_geometry(void *p, mtfs_block_geometry_t *g) { g->sector_size=512U;g->sector_count=1024U;g->erase_block_size=1U;return fake_call(p,5U); }
static mtfs_error_t f_trim(void *p, mtfs_lba_t l, mtfs_lba_t c) { (void)l;(void)c;return fake_call(p,6U); }
static const mtfs_block_device_ops_t fake_ops = { f_init,f_status,f_read,f_write,f_sync,f_geometry,f_trim };

typedef struct sentinel_transport_fake
{
    mtfs_sentinel_transport_snapshot_t snapshot;
    mtfs_error_t result;
} sentinel_transport_fake_t;

static mtfs_error_t fake_transport(void *p,
    mtfs_sentinel_transport_snapshot_t *snapshot)
{
    sentinel_transport_fake_t *fake = p;
    if (fake != NULL) {
        if (fake->result != MTFS_OK) return fake->result;
        *snapshot = fake->snapshot;
        return MTFS_OK;
    }
    snapshot->validity_mask =
        MTFS_SENTINEL_TRANSPORT_VALID_TRANSPORT_ERRORS;
    snapshot->flags = MTFS_SENTINEL_TRANSPORT_FLAG_COUNTERS_SATURATE;
    snapshot->transport_errors = 3U;
    return MTFS_OK;
}

typedef struct thread_args { mtfs_block_device_t *device; uint32_t count; } thread_args_t;
static void *read_thread(void *p)
{
    thread_args_t *a=p; uint8_t buffer[512]; uint32_t i;
    for (i=0U;i<a->count;++i) (void)mtfs_block_read(a->device,buffer,0U,1U);
    return NULL;
}
static void *write_thread(void *p)
{
    thread_args_t *a=p; uint8_t buffer[512] = {0}; uint32_t i;
    for (i=0U;i<a->count;++i) (void)mtfs_block_write(a->device,buffer,0U,1U);
    return NULL;
}

static mtfs_error_t sample_transport_frame(mtfs_sentinel_context_t *sampler,
    sentinel_fake_t *fake, const mtfs_sentinel_sample_metadata_t *metadata,
    mtfs_sentinel_feature_v1_t *frame)
{
    fake->clock_phase = 0U;
    fake->now += UINT64_C(1000);
    return mtfs_sentinel_sample(sampler, metadata, frame);
}

static int test_transport_deltas(mtfs_test_t *test,
    mtfs_sentinel_observer_t *observer, sentinel_fake_t *fake,
    const mtfs_sentinel_sample_metadata_t *metadata)
{
    sentinel_transport_fake_t transport;
    mtfs_sentinel_config_t config;
    mtfs_sentinel_context_t sampler;
    mtfs_sentinel_feature_v1_t frame;
    (void)memset(&transport, 0, sizeof(transport));
    (void)memset(&config, 0, sizeof(config));
    transport.result = MTFS_OK;
    transport.snapshot.reset_epoch = 4U;
    transport.snapshot.validity_mask = MTFS_SENTINEL_TRANSPORT_VALID_ALL;
    transport.snapshot.flags =
        MTFS_SENTINEL_TRANSPORT_FLAG_COUNTERS_SATURATE;
    transport.snapshot.transport_errors = 10U;
    transport.snapshot.transfer_timeouts = 20U;
    transport.snapshot.ready_timeouts = 30U;
    transport.snapshot.aborts = 40U;
    transport.snapshot.clock_errors = 50U;
    config.observer = observer;
    config.clock = fake_clock;
    config.clock_context = fake;
    config.target_id = 10U;
    config.transport_id = 20U;
    config.transport_sample = fake_transport;
    config.transport_context = &transport;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_init(&sampler, &config) == MTFS_OK &&
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA) != 0U,
            "transport sampler establishes a cumulative baseline")) return 1;

    transport.snapshot.transport_errors += 3U;
    transport.snapshot.transfer_timeouts += 2U;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.validity_mask & MTFS_SENTINEL_VALID_TRANSPORT) != 0U &&
            frame.transport.transport_errors == 3U &&
            frame.transport.transfer_timeouts == 2U &&
            frame.transport.ready_timeouts == 0U &&
            frame.transport.reset_epoch == 4U,
            "transport cumulative counters become checked frame deltas"))
        return 1;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK && frame.transport.transport_errors == 0U &&
            frame.transport.transfer_timeouts == 0U,
            "one transport error is not repeated in later frames")) return 1;

    ++transport.snapshot.reset_epoch;
    transport.snapshot.transport_errors = 1U;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U &&
            (frame.validity_mask & MTFS_SENTINEL_VALID_TRANSPORT) == 0U,
            "transport reset epoch starts a new session")) return 1;
    transport.snapshot.transport_errors += 2U;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK && frame.transport.transport_errors == 2U,
            "transport sampling recovers after reset baseline")) return 1;

    transport.snapshot.transport_errors = 0U;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U,
            "transport counter rollback starts a new session")) return 1;
    transport.snapshot.validity_mask &=
        ~MTFS_SENTINEL_TRANSPORT_VALID_ABORTS;
    transport.snapshot.aborts = UINT64_MAX;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U,
            "transport validity contract change starts a new session"))
        return 1;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.validity_mask & MTFS_SENTINEL_VALID_TRANSPORT) != 0U &&
            (frame.transport.validity_mask &
                MTFS_SENTINEL_TRANSPORT_VALID_ABORTS) == 0U &&
            frame.transport.aborts == 0U,
            "unavailable transport fields remain invalid and zero")) return 1;

    sampler.config.target_id = 11U;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U,
            "target identity change starts a new session")) return 1;
    sampler.config.transport_id = 21U;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U,
            "transport identity change starts a new session")) return 1;

    transport.result = MTFS_ERROR_NOT_READY;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U,
            "transport availability loss starts a new session")) return 1;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) == 0U &&
            (frame.validity_mask & MTFS_SENTINEL_VALID_TRANSPORT) == 0U,
            "stable transport unavailability is represented as invalid"))
        return 1;
    transport.result = MTFS_OK;
    if (!MTFS_TEST_CHECK(test,
            sample_transport_frame(&sampler, fake, metadata, &frame) ==
                MTFS_OK &&
            (frame.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U,
            "transport availability recovery starts a new session")) return 1;
    return 0;
}

static void make_window_frame(mtfs_sentinel_feature_v1_t *frame,
    uint64_t read_calls)
{
    (void)memset(frame, 0, sizeof(*frame));
    frame->version = MTFS_SENTINEL_SCHEMA_VERSION;
    frame->struct_size = (uint16_t)sizeof(*frame);
    frame->validity_mask = MTFS_SENTINEL_VALID_REQUIRED |
        MTFS_SENTINEL_VALID_TRANSPORT;
    frame->sample_count = 1U;
    frame->target_id = 1U;
    frame->transport_id = 2U;
    frame->media_generation = 1U;
    frame->operation[MTFS_SENTINEL_OPERATION_READ].calls = read_calls;
    frame->transport.reset_epoch = 1U;
    frame->transport.validity_mask = MTFS_SENTINEL_TRANSPORT_VALID_ALL;
}

static int test_window_semantics(mtfs_test_t *test)
{
    mtfs_sentinel_window_t window;
    mtfs_sentinel_feature_v1_t storage[2];
    mtfs_sentinel_feature_v1_t first;
    mtfs_sentinel_feature_v1_t second;
    mtfs_sentinel_feature_v1_t aggregate;
    make_window_frame(&first, 1U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_window_init(&window, storage, 2U) == MTFS_OK &&
            mtfs_sentinel_window_push(&window, &first) == MTFS_OK &&
            mtfs_sentinel_window_get(&window, &aggregate) == MTFS_OK &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA) != 0U,
            "partial window is insufficient")) return 1;
    make_window_frame(&second, 0U);
    second.flags = MTFS_SENTINEL_FLAG_NO_ACTIVITY;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_window_push(&window, &second) == MTFS_OK &&
            mtfs_sentinel_window_get(&window, &aggregate) == MTFS_OK &&
            aggregate.operation[MTFS_SENTINEL_OPERATION_READ].calls == 1U &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_NO_ACTIVITY) == 0U &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA) == 0U,
            "mixed active and idle frames produce an active ready window"))
        return 1;

    mtfs_sentinel_window_reset(&window);
    make_window_frame(&first, 0U);
    first.flags = MTFS_SENTINEL_FLAG_NO_ACTIVITY;
    second = first;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_window_push(&window, &first) == MTFS_OK &&
            mtfs_sentinel_window_push(&window, &second) == MTFS_OK &&
            mtfs_sentinel_window_get(&window, &aggregate) == MTFS_OK &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_NO_ACTIVITY) != 0U,
            "fully idle window reports no activity")) return 1;

    mtfs_sentinel_window_reset(&window);
    make_window_frame(&first, 1U);
    first.validity_mask &= ~MTFS_SENTINEL_VALID_TIMING_SYNC;
    second = first;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_window_push(&window, &first) == MTFS_OK &&
            mtfs_sentinel_window_push(&window, &second) == MTFS_OK &&
            mtfs_sentinel_window_get(&window, &aggregate) == MTFS_OK &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA) != 0U,
            "full window with missing required validity is insufficient"))
        return 1;

    mtfs_sentinel_window_reset(&window);
    make_window_frame(&first, 1U);
    make_window_frame(&second, 1U);
    first.transport.transport_errors = 2U;
    first.transport.aborts = 7U;
    second.transport.transport_errors = 3U;
    second.transport.aborts = UINT64_MAX;
    second.transport.validity_mask &=
        ~MTFS_SENTINEL_TRANSPORT_VALID_ABORTS;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_window_push(&window, &first) == MTFS_OK &&
            mtfs_sentinel_window_push(&window, &second) == MTFS_OK &&
            mtfs_sentinel_window_get(&window, &aggregate) == MTFS_OK &&
            aggregate.transport.transport_errors == 5U &&
            aggregate.transport.aborts == 0U &&
            (aggregate.transport.validity_mask &
                MTFS_SENTINEL_TRANSPORT_VALID_ABORTS) == 0U &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_COUNTER_SATURATED) == 0U,
            "transport window sums deltas and intersects field validity"))
        return 1;

    make_window_frame(&first, 0U);
    first.validity_mask = MTFS_SENTINEL_VALID_IDENTITY |
        MTFS_SENTINEL_VALID_MEDIA;
    first.flags = MTFS_SENTINEL_FLAG_DISCONTINUITY |
        MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA;
    first.removed_events = 1U;
    make_window_frame(&second, 1U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_window_push(&window, &first) == MTFS_OK &&
            mtfs_sentinel_window_push(&window, &second) == MTFS_OK &&
            mtfs_sentinel_window_get(&window, &aggregate) == MTFS_OK &&
            aggregate.removed_events == 1U &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA) != 0U,
            "transition remains in the dataset and blocks readiness")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_window_push(&window, &second) == MTFS_OK &&
            mtfs_sentinel_window_get(&window, &aggregate) == MTFS_OK &&
            aggregate.removed_events == 0U &&
            (aggregate.flags & (MTFS_SENTINEL_FLAG_DISCONTINUITY |
                MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA)) == 0U,
            "window recovers after transition frame eviction")) return 1;

    mtfs_sentinel_window_reset(&window);
    make_window_frame(&first, UINT64_MAX);
    make_window_frame(&second, 1U);
    first.sample_count = UINT32_MAX;
    first.inserted_events = UINT32_MAX;
    first.transport.transport_errors = UINT64_MAX;
    second.inserted_events = 1U;
    second.transport.transport_errors = 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_window_push(&window, &first) == MTFS_OK &&
            mtfs_sentinel_window_push(&window, &second) == MTFS_OK &&
            mtfs_sentinel_window_get(&window, &aggregate) == MTFS_OK &&
            aggregate.sample_count == UINT32_MAX &&
            aggregate.inserted_events == UINT32_MAX &&
            aggregate.operation[MTFS_SENTINEL_OPERATION_READ].calls ==
                UINT64_MAX &&
            aggregate.transport.transport_errors == UINT64_MAX &&
            (aggregate.flags & MTFS_SENTINEL_FLAG_COUNTER_SATURATED) != 0U,
            "window saturates sample, event, operation and transport sums"))
        return 1;
    return 0;
}

int test_sentinel(mtfs_test_t *test)
{
    sentinel_fake_t fake;
    mtfs_block_device_t downstream = {0};
    mtfs_sentinel_observer_t observer;
    mtfs_sentinel_observer_t rejected_observer;
    mtfs_sentinel_observer_config_t oc;
    mtfs_sentinel_observer_config_t rejected_config;
    mtfs_block_device_t *device;
    mtfs_sentinel_observer_snapshot_t snapshot;
    mtfs_sentinel_context_t sampler;
    mtfs_sentinel_config_t sc = {0};
    mtfs_sentinel_sample_metadata_t metadata = {1U,0U,1U,0U,0U};
    mtfs_sentinel_feature_v1_t frame, aggregate, storage[2];
    mtfs_sentinel_window_t window;
    pthread_t threads[4]; thread_args_t args;
    mtfs_block_status_t status; mtfs_block_geometry_t geometry;
    uint8_t buffer[1024]; uint32_t i;
    uint64_t locks_before;
    (void)memset(&frame, 0, sizeof(frame));
    frame.operation[0].timing_samples = UINT64_C(1);
    frame.operation[0].total_latency_us = UINT64_C(513);
    frame.operation[0].average_latency_us = UINT64_C(513);
    frame.operation[0].latency_histogram[10] = UINT64_C(1);
    if(!MTFS_TEST_CHECK(test,
        mtfs_sentinel_operation_timing_is_consistent(&frame.operation[0]),
        "accept timing total inside histogram bounds")) return 1;
    frame.operation[0].latency_histogram[10] = 0U;
    frame.operation[0].latency_histogram[9] = UINT64_C(1);
    if(!MTFS_TEST_CHECK(test,
        !mtfs_sentinel_operation_timing_is_consistent(&frame.operation[0]) &&
        !mtfs_sentinel_operation_timing_is_consistent(NULL),
        "reject timing total outside histogram bounds")) return 1;
    (void)memset(&fake,0,sizeof(fake));
    fake.blocked_operation = UINT32_MAX;
    (void)pthread_mutex_init(&fake.mutex,NULL);
    (void)pthread_mutex_init(&fake.downstream_mutex,NULL);
    (void)pthread_mutex_init(&fake.clock_mutex,NULL);
    (void)pthread_cond_init(&fake.downstream_condition,NULL);
    downstream.ops=&fake_ops; downstream.context=&fake;
    downstream.capabilities=MTFS_BLOCK_CAPABILITY_TRIM;
    oc.downstream=&downstream;oc.clock=fake_clock;oc.clock_context=&fake;
    oc.lock=fake_lock;oc.unlock=fake_unlock;oc.lock_context=&fake;
    rejected_config=oc;rejected_config.unlock=NULL;
    if (!MTFS_TEST_CHECK(test,
        mtfs_sentinel_observer_init(&rejected_observer,&rejected_config)==
            MTFS_ERROR_INVALID_ARGUMENT,
        "reject incomplete observer lock configuration deterministically"))
        return 1;
    if (!MTFS_TEST_CHECK(test,mtfs_sentinel_observer_init(&observer,&oc)==MTFS_OK,"initialize caller-owned Sentinel observer")) return 1;
    device=mtfs_sentinel_observer_block_device(&observer);fake.latency=7U;
    if (!MTFS_TEST_CHECK(test,
        mtfs_block_initialize(device)==MTFS_OK && mtfs_block_status(device,&status)==MTFS_OK &&
        mtfs_block_get_geometry(device,&geometry)==MTFS_OK && mtfs_block_read(device,buffer,0U,1U)==MTFS_OK &&
        mtfs_block_write(device,buffer,0U,1U)==MTFS_OK && mtfs_block_sync(device)==MTFS_OK &&
        mtfs_block_trim(device,0U,1U)==MTFS_OK,"forward every block operation")) return 1;
    for(i=0U;i<7U;++i) {
        uint32_t expected = (i == 5U) ? 4U : 1U;
        if(!MTFS_TEST_CHECK(test,fake.calls[i]==expected,"forward operations with standard range geometry checks")) return 1;
    }
    locks_before = fake.lock_calls;
    for (i=0U;i<9U;++i) (void)mtfs_block_read(device,buffer,0U,1U);
    for (i=0U;i<10U;++i) (void)mtfs_block_write(device,buffer,0U,1U);
    for (i=0U;i<2U;++i) (void)mtfs_block_sync(device);
    if(!MTFS_TEST_CHECK(test,
        fake.lock_calls-locks_before==UINT64_C(63) &&
        fake.lock_calls==fake.unlock_calls,
        "use deterministic 3/3/3 observer lock path (63 workload locks)"))
        return 1;
    fake.result=MTFS_ERROR_NO_MEDIA;
    if(!MTFS_TEST_CHECK(test,mtfs_block_read(device,buffer,0U,1U)==MTFS_ERROR_NO_MEDIA,"preserve downstream errors exactly")) return 1;
    fake.result=MTFS_OK;(void)mtfs_sentinel_observer_reset(&observer);
    for(i=0U;i<MTFS_SENTINEL_HISTOGRAM_BUCKETS-1U;++i){
        fake.latency=mtfs_sentinel_histogram_upper_us[i];(void)mtfs_block_read(device,buffer,0U,1U);
        if(!MTFS_TEST_CHECK(test,mtfs_sentinel_histogram_bucket(fake.latency)==i,"map every inclusive histogram boundary")) return 1;
    }
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_histogram_bucket(UINT64_MAX)==MTFS_SENTINEL_HISTOGRAM_BUCKETS-1U,"map large latency to overflow bucket")) return 1;
    (void)pthread_mutex_lock(&fake.mutex);
    observer.snapshot.read.sample_count=UINT64_MAX;
    (void)pthread_mutex_unlock(&fake.mutex);
    fake.latency=1U;(void)mtfs_block_read(device,buffer,0U,1U);
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_observer_get(&observer,&snapshot)==MTFS_OK && snapshot.read.sample_count==UINT64_MAX && (snapshot.flags&MTFS_SENTINEL_OBSERVER_FLAG_SATURATED)!=0U,"saturate timing counters and publish flag")) return 1;
    (void)mtfs_sentinel_observer_reset(&observer);
    fake.clock_fail=2U;
    if(!MTFS_TEST_CHECK(test,mtfs_block_sync(device)==MTFS_OK && mtfs_sentinel_observer_get(&observer,&snapshot)==MTFS_OK && snapshot.sync.invalid_samples==1U,"clock failure does not change I/O result")) return 1;
    fake.clock_phase=0U;fake.now=50U;fake.latency=UINT64_MAX;(void)mtfs_block_sync(device);
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_observer_get(&observer,&snapshot)==MTFS_OK && snapshot.sync.invalid_samples>=2U,"reject clock wrap or reversal")) return 1;

    sc.observer=&observer;sc.clock=fake_clock;sc.clock_context=&fake;sc.target_id=1U;sc.transport_id=2U;
    fake.clock_phase=0U;fake.now=1000U;fake.latency=100U;
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_init(&sampler,&sc)==MTFS_OK && mtfs_sentinel_sample(&sampler,&metadata,&frame)==MTFS_OK && (frame.flags&MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA)!=0U,"first sample establishes baseline")) return 1;
    fake.clock_phase=0U;
    (void)mtfs_block_read(device,buffer,0U,2U);(void)mtfs_block_write(device,buffer,2U,1U);
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_sample(&sampler,&metadata,&frame)==MTFS_OK && frame.operation[0].calls==1U && frame.operation[0].sectors_requested==2U && frame.operation[0].average_sectors_per_request_q16==(2U<<16U) && frame.operation[0].average_latency_us==100U && frame.operation[1].sector_completion_permille==1000U && frame.operation[1].average_latency_us==100U,"generate integer feature deltas")) return 1;
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_window_init(&window,storage,2U)==MTFS_OK && mtfs_sentinel_window_push(&window,&frame)==MTFS_OK,"push caller-owned window")) return 1;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,(frame.flags&MTFS_SENTINEL_FLAG_NO_ACTIVITY)!=0U && mtfs_sentinel_window_push(&window,&frame)==MTFS_OK && mtfs_sentinel_window_get(&window,&aggregate)==MTFS_OK && aggregate.sample_count==2U && aggregate.operation[0].calls==1U,"aggregate sliding window and no activity")) return 1;
    sampler.config.transport_sample=fake_transport;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,
        (frame.flags&MTFS_SENTINEL_FLAG_DISCONTINUITY)!=0U,
        "transport availability change invalidates the current session")) return 1;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,
        (frame.validity_mask&MTFS_SENTINEL_VALID_TRANSPORT)!=0U &&
        frame.transport.transport_errors==0U,
        "transport cumulative snapshot produces a non-repeated delta")) return 1;
    ++metadata.media_generation;++metadata.removed_events;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,
        (frame.flags&(MTFS_SENTINEL_FLAG_DISCONTINUITY|
            MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA))==
            (MTFS_SENTINEL_FLAG_DISCONTINUITY|
             MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA) &&
        frame.inserted_events==0U && frame.removed_events==1U &&
        mtfs_sentinel_window_push(&window,&frame)==MTFS_OK && window.count==1U,
        "media removal event survives discontinuity and resets window")) return 1;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,frame.inserted_events==0U &&
        frame.removed_events==0U,"media event delta is not repeated")) return 1;
    ++metadata.media_generation;++metadata.inserted_events;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,
        (frame.flags&MTFS_SENTINEL_FLAG_DISCONTINUITY)!=0U &&
        frame.inserted_events==1U && frame.removed_events==0U,
        "media insertion event survives discontinuity")) return 1;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    (void)mtfs_block_diagnostics_reset(device);
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,(frame.flags&MTFS_SENTINEL_FLAG_DISCONTINUITY)!=0U,"diagnostics reset epoch invalidates sample")) return 1;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    (void)mtfs_sentinel_observer_reset(&observer);
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,(frame.flags&MTFS_SENTINEL_FLAG_DISCONTINUITY)!=0U,"observer reset epoch invalidates sample")) return 1;
    fake.latency=1U;args.device=device;args.count=200U;
    for(i=0U;i<2U;++i)(void)pthread_create(&threads[i],NULL,read_thread,&args);
    for(i=2U;i<4U;++i)(void)pthread_create(&threads[i],NULL,write_thread,&args);
    for(i=0U;i<4U;++i)(void)pthread_join(threads[i],NULL);
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_observer_get(&observer,&snapshot)==MTFS_OK && snapshot.read.sample_count>=400U && snapshot.write.sample_count>=400U,"concurrent observer read/write updates and snapshot")) return 1;
    fake.blocked_operation=MTFS_SENTINEL_OPERATION_SYNC;
    fake.operation_entered=0U;fake.release_operation=0U;args.count=1U;
    (void)pthread_create(&threads[0],NULL,read_thread,&args);
    (void)pthread_mutex_lock(&fake.downstream_mutex);
    while(fake.operation_entered==0U)
        (void)pthread_cond_wait(&fake.downstream_condition,
            &fake.downstream_mutex);
    (void)pthread_mutex_unlock(&fake.downstream_mutex);
    if(!MTFS_TEST_CHECK(test,
        mtfs_sentinel_observer_get(&observer,&snapshot)==MTFS_OK &&
        mtfs_sentinel_observer_reset(&observer)==MTFS_ERROR_NOT_READY &&
        mtfs_sentinel_observer_deinit(&observer)==MTFS_ERROR_NOT_READY,
        "snapshot succeeds and reset/deinit refuse an active downstream call"))
        return 1;
    (void)pthread_mutex_lock(&fake.downstream_mutex);
    fake.release_operation=1U;
    (void)pthread_cond_broadcast(&fake.downstream_condition);
    (void)pthread_mutex_unlock(&fake.downstream_mutex);
    (void)pthread_join(threads[0],NULL);
    fake.blocked_operation=UINT32_MAX;
    (void)mtfs_sentinel_reset(&sampler);
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    (void)pthread_mutex_lock(&fake.mutex);
    ++observer.snapshot.read.total_latency_us;
    (void)pthread_mutex_unlock(&fake.mutex);
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,
        (frame.validity_mask&MTFS_SENTINEL_VALID_TIMING_READ)==0U &&
        (frame.flags&(MTFS_SENTINEL_FLAG_DISCONTINUITY|
            MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE))==
            (MTFS_SENTINEL_FLAG_DISCONTINUITY|
             MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE),
        "reject timing totals inconsistent with histogram")) return 1;
    if (test_transport_deltas(test, &observer, &fake, &metadata) != 0)
        return 1;
    if (test_window_semantics(test) != 0) return 1;
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_observer_deinit(&observer)==MTFS_OK && mtfs_sentinel_observer_block_device(&observer)==NULL,"safe observer deinit")) return 1;
    (void)pthread_cond_destroy(&fake.downstream_condition);
    (void)pthread_mutex_destroy(&fake.clock_mutex);
    (void)pthread_mutex_destroy(&fake.downstream_mutex);
    (void)pthread_mutex_destroy(&fake.mutex);return 0;
}
