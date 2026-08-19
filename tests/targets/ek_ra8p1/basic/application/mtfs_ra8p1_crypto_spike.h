#ifndef MTFS_RA8P1_CRYPTO_SPIKE_H
#define MTFS_RA8P1_CRYPTO_SPIKE_H

/* TEST ONLY - NOT FOR PRODUCTION. */

#ifndef MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
#define MTFS_RA8P1_CRYPTO_SPIKE_ENABLE (0)
#endif

int mtfs_ra8p1_crypto_spike_command(const char *line);
void mtfs_ra8p1_crypto_spike_banner(void);

#endif
