/*
 * microT-FS application configuration entry point.
 * Project-specific options will be added as their implementations land.
 */
#ifndef MTFS_CONFIG_H
#define MTFS_CONFIG_H

/* Maximum number of physical drives in the fixed block-device registry. */
#ifndef MTFS_BLOCK_REGISTRY_SIZE
#define MTFS_BLOCK_REGISTRY_SIZE (4U)
#endif

/* Keep FatFs formatting disabled unless an application explicitly enables it. */
#ifndef MTFS_FF_USE_MKFS
#define MTFS_FF_USE_MKFS (0)
#endif

/* Keep the normal FatFs read/write build unless explicitly configured. */
#ifndef MTFS_FF_FS_READONLY
#define MTFS_FF_FS_READONLY (0)
#endif

#endif /* MTFS_CONFIG_H */
