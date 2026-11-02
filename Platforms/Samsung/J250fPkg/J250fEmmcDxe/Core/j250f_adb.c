/* SPDX-License-Identifier: GPL-2.0-only
 * Bounded ADB v1 transport, legacy shell and SYNC v1 RAM uploads.
 * Wire format: AOSP packages/modules/adb docs/dev/protocol.md,
 * docs/dev/services.md and file_sync_protocol.h. No authentication in this
 * explicitly enabled development firmware. No Android services are implied.
 */
#include "j250f_adb.h"
#define FOUR(a, b, c, d)                                                       \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |              \
   ((uint32_t)(d) << 24))
#define CNXN FOUR('C', 'N', 'X', 'N')
#define OPEN FOUR('O', 'P', 'E', 'N')
#define OKAY FOUR('O', 'K', 'A', 'Y')
#define WRTE FOUR('W', 'R', 'T', 'E')
#define CLSE FOUR('C', 'L', 'S', 'E')
#define SEND FOUR('S', 'E', 'N', 'D')
#define DATA FOUR('D', 'A', 'T', 'A')
#define DONE FOUR('D', 'O', 'N', 'E')
#define QUIT FOUR('Q', 'U', 'I', 'T')
#define STAT FOUR('S', 'T', 'A', 'T')
#define FAIL FOUR('F', 'A', 'I', 'L')
static void cp(void *d, const void *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    ((uint8_t *)d)[i] = ((const uint8_t *)s)[i];
}
static uint32_t get(const uint8_t *p) {
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static void put(uint8_t *p, uint32_t v) {
  unsigned i;
  for (i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}
static int prefix(const char *s, const char *p) {
  while (*p)
    if (*s++ != *p++)
      return 0;
  return 1;
}
static uint32_t sum(const uint8_t *p, size_t n) {
  uint32_t s = 0;
  while (n--)
    s += *p++;
  return s;
}
static int packet(jadb *a, uint32_t cmd, uint32_t x, uint32_t y, const void *p,
                  size_t n) {
  unsigned i;
  uint8_t *b;
  if (n > JADB_MAX || a->tx_count == 8)
    return -1;
  i = (a->tx_head + a->tx_count) % 8;
  b = a->tx[i];
  put(b, cmd);
  put(b + 4, x);
  put(b + 8, y);
  put(b + 12, (uint32_t)n);
  put(b + 16, sum(p, n));
  put(b + 20, ~cmd);
  cp(b + 24, p, n);
  a->tx_len[i] = n + 24;
  a->tx_count++;
  return 0;
}
static void stream_reset(jadb *a) {
  a->local = a->remote = 0;
  a->service = a->awaiting = a->closing = 0;
  a->out_len = a->line_len = a->sync_used = a->sync_left = a->path_used = 0;
  a->sync_id = 0;
  a->uploading = 0;
}
void jadb_init(jadb *a, const jadb_ops *ops) {
  size_t i;
  for (i = 0; i < sizeof(*a); i++)
    ((uint8_t *)a)[i] = 0;
  a->ops = *ops;
  a->rx_need = 24;
  a->next_local = 1;
}
static int emit(jadb *a, const void *p, size_t n) {
  if (n > sizeof(a->output) - a->out_len)
    return -1;
  cp(a->output + a->out_len, p, n);
  a->out_len += n;
  return 0;
}
static int pump(jadb *a) {
  size_t n, i;
  if (a->awaiting)
    return 0;
  if (a->out_len) {
    n = a->out_len > JADB_MAX ? JADB_MAX : a->out_len;
    if (packet(a, WRTE, a->local, a->remote, a->output, n))
      return -1;
    for (i = n; i < a->out_len; i++)
      a->output[i - n] = a->output[i];
    a->out_len -= n;
    a->awaiting = 1;
  } else if (a->closing) {
    if (packet(a, CLSE, a->local, a->remote, 0, 0))
      return -1;
    stream_reset(a);
  }
  return 0;
}
static int execute(jadb *a, const char *cmd) {
  char b[4096];
  size_t n = a->ops.execute(a->ops.ctx, cmd, b, sizeof(b));
  if (n > sizeof(b))
    return -1;
  return emit(a, b, n);
}
static int sync_reply(jadb *a, uint32_t id, const void *b, size_t n) {
  uint8_t h[8];
  put(h, id);
  put(h + 4, (uint32_t)n);
  return emit(a, h, 8) || emit(a, b, n) ? -1 : 0;
}
static int sync_fail(jadb *a) {
  static const char msg[] = "RAM upload rejected";
  a->closing = 1;
  a->uploading = 0;
  return sync_reply(a, FAIL, msg, sizeof(msg) - 1);
}
static int sync_header(jadb *a) {
  uint8_t b[16];
  uint32_t mode = 0, size = 0;
  a->sync_id = get(a->sync_header);
  a->sync_length = get(a->sync_header + 4);
  a->sync_left = a->sync_length;
  a->path_used = 0;
  if (a->sync_id == QUIT) {
    a->closing = 1;
    a->sync_left = 0;
    return 0;
  }
  if (a->sync_id == DONE) {
    if (!a->uploading || a->ops.done(a->ops.ctx))
      return sync_fail(a);
    a->uploading = 0;
    a->sync_left = 0;
    put(b, OKAY);
    put(b + 4, 0);
    return emit(a, b, 8);
  }
  if (a->sync_id == DATA) {
    if (!a->uploading || !a->sync_length || a->sync_length > 65536)
      return sync_fail(a);
    return 0;
  }
  if ((a->sync_id != SEND && a->sync_id != STAT) || !a->sync_length ||
      a->sync_length >= sizeof(a->path))
    return sync_fail(a);
  (void)mode;
  (void)size;
  return 0;
}
static int sync_bytes(jadb *a, const uint8_t *p, size_t n) {
  while (n && !a->closing) {
    size_t k;
    if (!a->sync_left) {
      k = 8 - a->sync_used;
      if (k > n)
        k = n;
      cp(a->sync_header + a->sync_used, p, k);
      a->sync_used += k;
      p += k;
      n -= k;
      if (a->sync_used < 8)
        continue;
      a->sync_used = 0;
      if (sync_header(a))
        return -1;
      if (!a->sync_left)
        continue;
    }
    k = a->sync_left;
    if (k > n)
      k = n;
    if (a->sync_id == DATA) {
      if (a->ops.data(a->ops.ctx, p, k))
        return sync_fail(a);
    } else {
      cp(a->path + a->path_used, p, k);
      a->path_used += k;
    }
    p += k;
    n -= k;
    a->sync_left -= (uint32_t)k;
    if (!a->sync_left && (a->sync_id == SEND || a->sync_id == STAT)) {
      size_t i;
      uint8_t b[16];
      uint32_t mode = 0, size = 0;
      a->path[a->path_used] = 0;
      for (i = 0; i < a->path_used; i++)
        if (!a->path[i])
          return sync_fail(a);
      if (a->sync_id == SEND) {
        for (i = 0; i < a->path_used; i++)
          if (a->path[i] == ',') {
            a->path[i] = 0;
            break;
          }
        if (a->ops.begin(a->ops.ctx, a->path))
          return sync_fail(a);
        a->uploading = 1;
      } else {
        (void)a->ops.stat(a->ops.ctx, a->path, &mode, &size);
        put(b, STAT);
        put(b + 4, mode);
        put(b + 8, size);
        put(b + 12, 0);
        if (emit(a, b, 16))
          return -1;
      }
    }
  }
  return 0;
}
static int shell_bytes(jadb *a, const uint8_t *p, size_t n) {
  while (n--) {
    uint8_t c = *p++;
    if (c == '\r')
      continue;
    if (c == '\n') {
      if (emit(a, "\r\n", 2))
        return -1;
      a->line[a->line_len] = 0;
      if (prefix(a->line, "exit") && a->line_len == 4) {
        a->closing = 1;
        a->line_len = 0;
        return 0;
      }
      if (execute(a, a->line))
        return -1;
      a->line_len = 0;
      if (emit(a, "J250F-UEFI> ", 12))
        return -1;
    } else if (c == 8 || c == 127) {
      if (a->line_len) {
        a->line_len--;
        if (emit(a, "\b \b", 3))
          return -1;
      }
    } else if (c >= 32 && c < 127) {
      if (a->line_len + 1 >= sizeof(a->line))
        return -1;
      a->line[a->line_len++] = (char)c;
      if (emit(a, &c, 1))
        return -1;
    }
  }
  return 0;
}
static int dispatch(jadb *a) {
  uint8_t *b = a->rx;
  uint32_t c = get(b), x = get(b + 4), y = get(b + 8);
  size_t n = get(b + 12);
  char *p = (char *)b + 24;
  if (sum((uint8_t *)p, n) != get(b + 16))
    return -1;
  if (c == CNXN) {
    static const char banner[] = "device::ro.product.name=J250F;ro.product."
                                 "model=J250F-UEFI;ro.product.device=j250f;";
    if (x < 0x01000000 || y < JADB_MAX)
      return -1;
    stream_reset(a);
    a->connected = 1;
    return packet(a, CNXN, 0x01000000, JADB_MAX, banner, sizeof(banner));
  }
  if (!a->connected)
    return -1;
  if (c == OPEN) {
    const char *cmd;
    if (!n || p[n - 1] || a->local)
      return packet(a, CLSE, 0, x, 0, 0);
    a->local = a->next_local++;
    if (!a->local)
      a->local = a->next_local++;
    a->remote = x;
    if (prefix(p, "sync:") && n == 6)
      a->service = 2;
    else if (prefix(p, "shell:") || prefix(p, "exec:"))
      a->service = 1;
    else {
      uint32_t l = a->local;
      stream_reset(a);
      return packet(a, CLSE, l, x, 0, 0);
    }
    if (packet(a, OKAY, a->local, a->remote, 0, 0))
      return -1;
    if (a->service == 1) {
      cmd = p + (p[0] == 's' ? 6 : 5);
      if (*cmd) {
        if (execute(a, cmd))
          return -1;
        a->closing = 1;
      } else if (emit(a, "J250F-UEFI> ", 12))
        return -1;
    }
    return pump(a);
  }
  if (x != a->remote || y != a->local || !a->local)
    return c == CLSE ? 0 : packet(a, CLSE, 0, x, 0, 0);
  if (c == CLSE) {
    uint32_t l = a->local, r = a->remote;
    stream_reset(a);
    return packet(a, CLSE, l, r, 0, 0);
  }
  if (c == OKAY) {
    a->awaiting = 0;
    return pump(a);
  }
  if (c == WRTE) {
    if (packet(a, OKAY, a->local, a->remote, 0, 0))
      return -1;
    if (a->service == 2) {
      if (sync_bytes(a, (uint8_t *)p, n))
        return -1;
    } else if (shell_bytes(a, (uint8_t *)p, n))
      return -1;
    return pump(a);
  }
  return -1;
}
int jadb_feed(jadb *a, const uint8_t *p, size_t n) {
  while (n) {
    size_t k = a->rx_need - a->rx_used;
    if (k > n)
      k = n;
    cp(a->rx + a->rx_used, p, k);
    a->rx_used += k;
    p += k;
    n -= k;
    if (a->rx_used != a->rx_need)
      continue;
    if (a->rx_need == 24) {
      uint32_t size = get(a->rx + 12);
      if (get(a->rx + 20) != ~get(a->rx) || size > JADB_MAX)
        return -1;
      a->rx_need = 24 + size;
      if (size)
        continue;
    }
    if (dispatch(a))
      return -1;
    a->rx_used = 0;
    a->rx_need = 24;
  }
  return 0;
}
const uint8_t *jadb_tx(jadb *a, size_t *n) {
  if (!a->tx_count) {
    *n = 0;
    return 0;
  }
  *n = a->tx_len[a->tx_head];
  return a->tx[a->tx_head];
}
void jadb_tx_done(jadb *a) {
  if (a->tx_count) {
    a->tx_head = (a->tx_head + 1) % 8;
    a->tx_count--;
  }
}
