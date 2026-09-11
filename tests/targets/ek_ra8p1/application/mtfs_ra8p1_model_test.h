#ifndef MTFS_RA8P1_MODEL_TEST_H
#define MTFS_RA8P1_MODEL_TEST_H

#include "mtfs_media.h"
#include "mtfs_block_device.h"

int mtfs_ra8p1_model_command(const char *line,
    const mtfs_media_context_t *media, mtfs_block_device_t *device,
    int *registered);

#endif
