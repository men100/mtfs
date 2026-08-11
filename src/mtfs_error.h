/* Common status values for the initial microT-FS API. */
#ifndef MTFS_ERROR_H
#define MTFS_ERROR_H

typedef enum mtfs_error
{
    MTFS_OK = 0,
    MTFS_ERROR_INVALID_ARGUMENT = -1,
    MTFS_ERROR_NOT_SUPPORTED = -2,
    MTFS_ERROR_IO = -3,
    MTFS_ERROR_NOT_READY = -4,
    MTFS_ERROR_NO_MEDIA = -5,
    MTFS_ERROR_WRITE_PROTECTED = -6,
    MTFS_ERROR_OUT_OF_RANGE = -7,
    MTFS_ERROR_ALREADY_EXISTS = -8,
    MTFS_ERROR_NOT_FOUND = -9
} mtfs_error_t;

#endif /* MTFS_ERROR_H */
