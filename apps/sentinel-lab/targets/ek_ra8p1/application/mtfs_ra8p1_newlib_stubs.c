#include <errno.h>
#include <stddef.h>
#include <reent.h>
#include <stdint.h>

/*
 * The C++ TFLM link pulls newlib's abort/stdio support.
 * This target has no POSIX file-descriptor layer: its console and storage use
 * T-Monitor and FatFs directly.  Supply bounded, allocation-free failure
 * stubs instead of linking libnosys (which emits linker warnings).
 */

static volatile uint32_t heap_calls;

extern void * __real__malloc_r(struct _reent * reent, size_t size);
extern void * __real__realloc_r(struct _reent * reent, void * pointer,
    size_t size);
extern void __real__free_r(struct _reent * reent, void * pointer);

void * __wrap__malloc_r(struct _reent * reent, size_t size)
{
    ++heap_calls;
    return __real__malloc_r(reent, size);
}

void * __wrap__realloc_r(struct _reent * reent, void * pointer, size_t size)
{
    ++heap_calls;
    return __real__realloc_r(reent, pointer, size);
}

void __wrap__free_r(struct _reent * reent, void * pointer)
{
    ++heap_calls;
    __real__free_r(reent, pointer);
}

uint32_t mtfs_ra8p1_heap_call_count(void)
{
    return heap_calls;
}

int _close(int file)
{
    (void) file;
    errno = ENOSYS;
    return -1;
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int sig)
{
    (void) pid;
    (void) sig;
    errno = EINVAL;
    return -1;
}

long _lseek(int file, long offset, int whence)
{
    (void) file;
    (void) offset;
    (void) whence;
    errno = ENOSYS;
    return -1L;
}

int _read(int file, void * buffer, size_t length)
{
    (void) file;
    (void) buffer;
    (void) length;
    errno = ENOSYS;
    return -1;
}

int _write(int file, const void * buffer, size_t length)
{
    (void) file;
    (void) buffer;
    (void) length;
    errno = ENOSYS;
    return -1;
}

__attribute__((noreturn)) void _exit(int status)
{
    (void) status;

    for (;;)
    {
        __asm volatile ("wfe");
    }
}
