#include "test_sentinel.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "mtfs_sentinel.h"

typedef struct sentinel_fake {
    pthread_mutex_t mutex;
    pthread_mutex_t clock_mutex;
    mtfs_error_t result;
    uint64_t now;
    uint64_t latency;
    uint32_t clock_phase;
    uint32_t clock_fail;
    uint32_t calls[7];
} sentinel_fake_t;

static mtfs_error_t fake_lock(void *p) { return pthread_mutex_lock(p) == 0 ? MTFS_OK : MTFS_ERROR_INVALID_STATE; }
static void fake_unlock(void *p) { (void)pthread_mutex_unlock(p); }
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
    (void)pthread_mutex_lock(&f->mutex); ++f->calls[operation];
    result=f->result; (void)pthread_mutex_unlock(&f->mutex); return result;
}
static mtfs_error_t f_init(void *p) { return fake_call(p,0U); }
static mtfs_error_t f_status(void *p, mtfs_block_status_t *s) { *s=MTFS_BLOCK_STATUS_MEDIA_PRESENT;return fake_call(p,1U); }
static mtfs_error_t f_read(void *p, void *b, mtfs_lba_t l, uint32_t c) { (void)b;(void)l;(void)c;return fake_call(p,2U); }
static mtfs_error_t f_write(void *p, const void *b, mtfs_lba_t l, uint32_t c) { (void)b;(void)l;(void)c;return fake_call(p,3U); }
static mtfs_error_t f_sync(void *p) { return fake_call(p,4U); }
static mtfs_error_t f_geometry(void *p, mtfs_block_geometry_t *g) { g->sector_size=512U;g->sector_count=1024U;g->erase_block_size=1U;return fake_call(p,5U); }
static mtfs_error_t f_trim(void *p, mtfs_lba_t l, mtfs_lba_t c) { (void)l;(void)c;return fake_call(p,6U); }
static const mtfs_block_device_ops_t fake_ops = { f_init,f_status,f_read,f_write,f_sync,f_geometry,f_trim };

static mtfs_error_t fake_transport(void *p,
    mtfs_sentinel_transport_feature_t *feature)
{
    (void)p;
    feature->validity_mask = 1U;
    feature->flags = 2U;
    feature->counters[0] = 3U;
    return MTFS_OK;
}

typedef struct thread_args { mtfs_block_device_t *device; uint32_t count; } thread_args_t;
static void *read_thread(void *p)
{
    thread_args_t *a=p; uint8_t buffer[512]; uint32_t i;
    for (i=0U;i<a->count;++i) (void)mtfs_block_read(a->device,buffer,0U,1U);
    return NULL;
}

int test_sentinel(mtfs_test_t *test)
{
    sentinel_fake_t fake;
    mtfs_block_device_t downstream = {0};
    mtfs_sentinel_observer_t observer;
    mtfs_sentinel_observer_config_t oc;
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
    (void)pthread_mutex_init(&fake.mutex,NULL);
    (void)pthread_mutex_init(&fake.clock_mutex,NULL);
    downstream.ops=&fake_ops; downstream.context=&fake;
    downstream.capabilities=MTFS_BLOCK_CAPABILITY_TRIM;
    oc.downstream=&downstream;oc.clock=fake_clock;oc.clock_context=&fake;
    oc.lock=fake_lock;oc.unlock=fake_unlock;oc.lock_context=&fake.mutex;
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
    if(!MTFS_TEST_CHECK(test,(frame.validity_mask&MTFS_SENTINEL_VALID_TRANSPORT)!=0U && frame.transport.counters[0]==3U,"include optional transport supplement without vendor headers")) return 1;
    ++metadata.media_generation;(void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,(frame.flags&MTFS_SENTINEL_FLAG_DISCONTINUITY)!=0U && mtfs_sentinel_window_push(&window,&frame)==MTFS_OK && window.count==1U,"media change resets window")) return 1;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    (void)mtfs_block_diagnostics_reset(device);
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,(frame.flags&MTFS_SENTINEL_FLAG_DISCONTINUITY)!=0U,"diagnostics reset epoch invalidates sample")) return 1;
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    (void)mtfs_sentinel_observer_reset(&observer);
    (void)mtfs_sentinel_sample(&sampler,&metadata,&frame);
    if(!MTFS_TEST_CHECK(test,(frame.flags&MTFS_SENTINEL_FLAG_DISCONTINUITY)!=0U,"observer reset epoch invalidates sample")) return 1;
    fake.latency=1U;args.device=device;args.count=200U;
    for(i=0U;i<4U;++i)(void)pthread_create(&threads[i],NULL,read_thread,&args);
    for(i=0U;i<4U;++i)(void)pthread_join(threads[i],NULL);
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_observer_get(&observer,&snapshot)==MTFS_OK && snapshot.read.sample_count>=800U,"concurrent observer updates and snapshot")) return 1;
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
    if(!MTFS_TEST_CHECK(test,mtfs_sentinel_observer_deinit(&observer)==MTFS_OK && mtfs_sentinel_observer_block_device(&observer)==NULL,"safe observer deinit")) return 1;
    (void)pthread_mutex_destroy(&fake.clock_mutex);
    (void)pthread_mutex_destroy(&fake.mutex);return 0;
}
