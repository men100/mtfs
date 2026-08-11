/* Common status values for the initial microT-FS API. */
#ifndef MTFS_ERROR_H
#define MTFS_ERROR_H

typedef enum mtfs_error
{
    MTFS_OK = 0,
    MTFS_ERROR_INVALID_ARGUMENT = -1,
    MTFS_ERROR_NOT_SUPPORTED = -2,
    MTFS_ERROR_IO = -3
} mtfs_error_t;

#endif /* MTFS_ERROR_H */
