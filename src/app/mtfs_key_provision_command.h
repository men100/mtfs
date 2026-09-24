/** @file mtfs_key_provision_command.h
 * @brief Shared key-provision console command contract. / key provision consoleの共通command契約。
 * @ingroup mtfs_app */
#ifndef MTFS_KEY_PROVISION_COMMAND_H
#define MTFS_KEY_PROVISION_COMMAND_H

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mtfs_key_provision_command
{
    MTFS_KEY_PROVISION_COMMAND_NONE = 0,
    MTFS_KEY_PROVISION_COMMAND_INITIAL,
    MTFS_KEY_PROVISION_COMMAND_REPLACE,
    MTFS_KEY_PROVISION_COMMAND_LEGACY_UPDATE
} mtfs_key_provision_command_t;

/** Parse only commands that can alter or discuss the fleet-key record.
 * `update-xmodem` remains recognized solely to provide fail-closed migration
 * guidance; callers must not start XMODEM or flash operations for that value. */
static inline mtfs_key_provision_command_t mtfs_key_provision_command_parse(
    const char *line)
{
    if (line == NULL) {
        return MTFS_KEY_PROVISION_COMMAND_NONE;
    }
    if (strcmp(line, "provision-xmodem") == 0) {
        return MTFS_KEY_PROVISION_COMMAND_INITIAL;
    }
    if (strcmp(line, "provision-xmodem replace") == 0) {
        return MTFS_KEY_PROVISION_COMMAND_REPLACE;
    }
    if (strcmp(line, "update-xmodem") == 0) {
        return MTFS_KEY_PROVISION_COMMAND_LEGACY_UPDATE;
    }
    return MTFS_KEY_PROVISION_COMMAND_NONE;
}

static inline const char *mtfs_key_provision_legacy_guidance(void)
{
    return "BLOCKED: versioned key rotation is unsupported.\n"
           "Use \"provision-xmodem replace\" for single-key replacement.\n";
}

#ifdef __cplusplus
}
#endif

#endif /* MTFS_KEY_PROVISION_COMMAND_H */
