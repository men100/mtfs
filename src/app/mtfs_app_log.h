#ifndef MTFS_APP_LOG_H
#define MTFS_APP_LOG_H

#include <string.h>

/*
 * Runtime application logging is independent of the compiler build type.
 * アプリケーションの実行時ログ設定はコンパイラのbuild種別から独立している。
 */
typedef enum mtfs_app_log_level
{
    MTFS_APP_LOG_OFF = 0,
    MTFS_APP_LOG_ERROR,
    MTFS_APP_LOG_INFO,
    MTFS_APP_LOG_DEBUG
} mtfs_app_log_level_t;

static inline const char *mtfs_app_log_level_name(mtfs_app_log_level_t level)
{
    switch (level) {
    case MTFS_APP_LOG_OFF:
        return "off";
    case MTFS_APP_LOG_ERROR:
        return "error";
    case MTFS_APP_LOG_INFO:
        return "info";
    case MTFS_APP_LOG_DEBUG:
        return "debug";
    default:
        return "info";
    }
}

static inline int mtfs_app_log_enabled(
    mtfs_app_log_level_t current, mtfs_app_log_level_t message)
{
    return current >= message;
}

/* Returns 1 for a valid command, -1 for invalid syntax, and 0 otherwise. */
static inline int mtfs_app_log_parse_command(
    const char *line, mtfs_app_log_level_t *level, int *is_query)
{
    const char *argument;

    if ((line == NULL) || (level == NULL) || (is_query == NULL)) {
        return -1;
    }
    if (strcmp(line, "log-level") == 0) {
        *is_query = 1;
        return 1;
    }
    if (strncmp(line, "log-level ", 10U) != 0) {
        return 0;
    }
    argument = line + 10U;
    *is_query = 0;
    if (strcmp(argument, "off") == 0) {
        *level = MTFS_APP_LOG_OFF;
    } else if (strcmp(argument, "error") == 0) {
        *level = MTFS_APP_LOG_ERROR;
    } else if (strcmp(argument, "info") == 0) {
        *level = MTFS_APP_LOG_INFO;
    } else if (strcmp(argument, "debug") == 0) {
        *level = MTFS_APP_LOG_DEBUG;
    } else {
        return -1;
    }
    return 1;
}

#endif /* MTFS_APP_LOG_H */
