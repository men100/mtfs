#ifndef MTFS_RA8P1_CRYPTO_SPIKE_H
#define MTFS_RA8P1_CRYPTO_SPIKE_H

/* RSIP Compatibility Mode provisioned-key validation commands. */

#ifndef MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
#define MTFS_RA8P1_CRYPTO_SPIKE_ENABLE (0)
#endif

int mtfs_ra8p1_crypto_spike_command(const char *line);
void mtfs_ra8p1_crypto_spike_banner(void);

#endif
