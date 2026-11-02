/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J250F_ADB_H
#define J250F_ADB_H
#include <stddef.h>
#include <stdint.h>
#define JADB_MAX 4096u
#define JADB_FILE_MAX (16u * 1024u * 1024u)
typedef struct {
  void *ctx;
  size_t (*execute)(void *, const char *, char *, size_t);
  int (*begin)(void *, const char *);
  int (*data)(void *, const uint8_t *, size_t);
  int (*done)(void *);
  int (*stat)(void *, const char *, uint32_t *, uint32_t *);
} jadb_ops;
typedef struct {
  jadb_ops ops;
  uint8_t rx[JADB_MAX + 24], tx[8][JADB_MAX + 24], output[8192];
  size_t rx_used, rx_need, tx_len[8], out_len;
  unsigned tx_head, tx_count;
  uint32_t local, remote, next_local;
  unsigned connected, service, awaiting, closing;
  char line[512];
  size_t line_len;
  uint8_t sync_header[8];
  size_t sync_used;
  uint32_t sync_id, sync_left, sync_length;
  char path[256];
  size_t path_used;
  unsigned uploading;
} jadb;
void jadb_init(jadb *, const jadb_ops *);
int jadb_feed(jadb *, const uint8_t *, size_t);
const uint8_t *jadb_tx(jadb *, size_t *);
void jadb_tx_done(jadb *);
#endif
