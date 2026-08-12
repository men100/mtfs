#ifndef MTFS_TEST_H
#define MTFS_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mtfs_test_event
{
    MTFS_TEST_EVENT_BEGIN = 0,
    MTFS_TEST_EVENT_CHECK_FAILED,
    MTFS_TEST_EVENT_FINISH
} mtfs_test_event_t;

typedef void (*mtfs_test_reporter_t)(
    void *context,
    mtfs_test_event_t event,
    const char *test_name,
    const char *file,
    int line,
    const char *message,
    unsigned int checks,
    unsigned int failures);

typedef struct mtfs_test
{
    const char *name;
    unsigned int checks;
    unsigned int failures;
    mtfs_test_reporter_t reporter;
    void *reporter_context;
} mtfs_test_t;

void mtfs_test_begin(
    mtfs_test_t *test,
    const char *name,
    mtfs_test_reporter_t reporter,
    void *reporter_context);
int mtfs_test_check(
    mtfs_test_t *test,
    int condition,
    const char *file,
    int line,
    const char *message);
int mtfs_test_finish(const mtfs_test_t *test);

#define MTFS_TEST_CHECK(test, condition, message) \
    mtfs_test_check((test), (condition), __FILE__, __LINE__, (message))

#ifdef __cplusplus
}
#endif

#endif /* MTFS_TEST_H */
