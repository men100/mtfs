/** @file mtfs_error.h
 * @brief Common status values. / 共通のstatus値。
 * @details MTFS_OK is zero; all errors are negative and functions do not use errno.
 * / MTFS_OKは0、すべてのerrorは負値で表し、errnoは使用しない。
 * @ingroup mtfs_core */
#ifndef MTFS_ERROR_H
#define MTFS_ERROR_H

/** @addtogroup mtfs_core
 * @{ */

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
    MTFS_ERROR_NOT_FOUND = -9,
    MTFS_ERROR_MALFORMED_FORMAT = -10,
    MTFS_ERROR_AUTHENTICATION = -11,
    MTFS_ERROR_BUFFER_TOO_SMALL = -12,
    MTFS_ERROR_INVALID_STATE = -13,
    MTFS_ERROR_OVERFLOW = -14,
    MTFS_ERROR_UNSUPPORTED_FORMAT = -15,
    MTFS_ERROR_CRYPTO = -16
} mtfs_error_t;

/** @} */

#endif /* MTFS_ERROR_H */
