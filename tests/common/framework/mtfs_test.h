#ifndef MTFS_TEST_H
#define MTFS_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mtfs_test
{
    const char *name;
    unsigned int checks;
    unsigned int failures;
} mtfs_test_t;

void mtfs_test_begin(mtfs_test_t *test, const char *name);
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
