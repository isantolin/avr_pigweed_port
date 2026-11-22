/*
 * Host-only simavr runner exposing AVR USART channels (PTY or TCP), a
 * synchronous JSON-lines TCP monitor channel for Pigweed pw_emu control and
 * property access, and optional native simavr GDB RSP support.
 *
 * The AVR firmware remains unchanged; all emulation code runs on the host.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "avr_adc.h"
#include "avr_eeprom.h"
#include "avr_ioport.h"
#include "avr_uart.h"
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_gdb.h"
#include "sim_hex.h"
#include "sim_io.h"
#include "sim_irq.h"

enum {
  MAX_UART_CHANNELS = 4,
  MAX_MONITOR_CLIENTS = 4,
  UART_QUEUE_CAPACITY = 65536,
  DRAIN_BATCH_BYTES = 64,
  DEFAULT_CYCLES_PER_UART_BYTE = 1500,
  BATCH_INSTRUCTION_CYCLES = 2048,
  PEAK_STACK_SNAPSHOT_BYTES = 96,
  CRASH_STACK_DUMP_BYTES = 32,
  MONITOR_LINE_MAX = 4096,
  MONITOR_RESP_MAX = 16384,
  ADC_CHANNEL_COUNT = 17,  // ADC0..ADC15 + TEMP
};

typedef enum {
  CHAN_TYPE_PTY = 0,
  CHAN_TYPE_TCP = 1,
} uart_backend_t;

typedef struct {
  char name[32];
  char uart_id;
  uart_backend_t backend;
  int pty_master;
  int pty_slave;
  char pty_path[256];
  int tcp_listen_fd;
  int tcp_client_fd;
  uint16_t tcp_port;
  bool xon;
  avr_irq_t *rx_irq;
  uint8_t tx_queue[UART_QUEUE_CAPACITY];
  size_t tx_head;
  size_t tx_tail;
  uint8_t rx_queue[UART_QUEUE_CAPACITY];
  size_t rx_head;
  size_t rx_tail;
  uint64_t tx_bytes;
  uint64_t rx_bytes;
  uint64_t rx_dropped;
} uart_channel_t;

typedef struct {
  int fd;
  char rx_buf[MONITOR_LINE_MAX];
  size_t rx_len;
} monitor_client_t;

typedef struct {
  int listen_fd;
  uint16_t port;
  monitor_client_t clients[MAX_MONITOR_CLIENTS];
} monitor_server_t;

typedef struct {
  uint16_t min_sp;
  avr_flashaddr_t min_sp_pc;
  uint8_t min_sp_stack[PEAK_STACK_SNAPSHOT_BYTES];
  size_t min_sp_stack_len;
  bool crashed;
  avr_flashaddr_t crash_pc;
  uint16_t crash_sp;
  uint8_t crash_sreg;
  avr_cycle_count_t crash_cycle;
  uint8_t crash_stack[CRASH_STACK_DUMP_BYTES];
  size_t crash_stack_len;
} diagnostics_state_t;

typedef struct {
  char name[32];
  char uart_id;
  uart_backend_t backend;
} uart_spec_t;

static volatile sig_atomic_t g_stop = 0;

static int g_failure = 0;
static bool g_paused = false;
static uint32_t g_cycles_per_uart_byte = DEFAULT_CYCLES_PER_UART_BYTE;
static uint32_t g_vcc_mv = 5000;
static uint32_t g_avcc_mv = 5000;
static uint32_t g_aref_mv = 5000;
static bool g_has_reset_pc = false;
static avr_flashaddr_t g_reset_pc = 0;
static int g_log_level = LOG_WARNING;
static int g_gdb_port = 0;
static bool g_crash_logged = false;
static char g_mcu_name[64] = "atmega328p";

static uart_channel_t g_uarts[MAX_UART_CHANNELS];
static size_t g_uart_count = 0;
static monitor_server_t g_monitor;
static diagnostics_state_t g_diag;
static uint32_t g_adc_mv[ADC_CHANNEL_COUNT];

/* ---- Safe UTF-8 logger -------------------------------------------------- */

static void safe_simavr_logger(struct avr_t *avr, const int level,
                               const char *format, va_list ap) {
  const int threshold = avr ? (int)avr->log : g_log_level;
  if (level > threshold) {
    return;
  }
  char buffer[1024];
  const int written = vsnprintf(buffer, sizeof(buffer), format, ap);
  if (written <= 0) {
    return;
  }
  const size_t raw_len = ((size_t)written < sizeof(buffer))
                             ? (size_t)written
                             : (sizeof(buffer) - 1U);
  buffer[raw_len] = '\0';
  if (strstr(buffer, "avr_sadly_crashed") != NULL) {
    g_crash_logged = true;
  }
  char clean[1024];
  size_t out_len = 0;
  for (size_t i = 0; i < raw_len && out_len + 1U < sizeof(clean); ++i) {
    const unsigned char uc = (unsigned char)buffer[i];
    // Strip ANSI CSI color escapes (e.g. "\x1b[31m") emitted by libsimavr.
    if (uc == 0x1BU && i + 1U < raw_len && buffer[i + 1U] == '[') {
      i += 2U;
      while (i < raw_len &&
             !((unsigned char)buffer[i] >= 0x40U &&
               (unsigned char)buffer[i] <= 0x7EU)) {
        ++i;
      }
      continue;
    }
    if (uc >= 0x80U || (uc < 0x20U && uc != '\n' && uc != '\r' && uc != '\t')) {
      clean[out_len++] = '?';
    } else {
      clean[out_len++] = (char)uc;
    }
  }
  clean[out_len] = '\0';
  fputs(clean, stderr);
  fflush(stderr);
}

/* ---- Signals ------------------------------------------------------------ */

static void stop_handler(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

/* ---- Atomic file helper ------------------------------------------------- */

static int write_file_atomic(const char *path, const char *content,
                             size_t len) {
  char tmp_path[1024];
  const int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path,
                         (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
    fprintf(stderr, "Path too long for atomic write: %s\n", path);
    return -1;
  }
  const int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    fprintf(stderr, "Unable to open %s: %s\n", tmp_path, strerror(errno));
    return -1;
  }
  size_t offset = 0;
  while (offset < len) {
    const ssize_t w = write(fd, content + offset, len - offset);
    if (w > 0) {
      offset += (size_t)w;
    } else if (w < 0 && errno == EINTR) {
      continue;
    } else {
      fprintf(stderr, "Write failed for %s: %s\n", tmp_path, strerror(errno));
      close(fd);
      unlink(tmp_path);
      return -1;
    }
  }
  if (fsync(fd) != 0 && errno != EINVAL && errno != EROFS) {
    fprintf(stderr, "fsync failed for %s: %s\n", tmp_path, strerror(errno));
    close(fd);
    unlink(tmp_path);
    return -1;
  }
  if (close(fd) != 0) {
    fprintf(stderr, "close failed for %s: %s\n", tmp_path, strerror(errno));
    unlink(tmp_path);
    return -1;
  }
  if (rename(tmp_path, path) != 0) {
    fprintf(stderr, "rename %s -> %s failed: %s\n", tmp_path, path,
            strerror(errno));
    unlink(tmp_path);
    return -1;
  }
  return 0;
}

/* ---- JSON helpers ------------------------------------------------------- */

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
  bool overflow;
} strbuf_t;

static void sb_init(strbuf_t *sb, char *buf, size_t cap) {
  sb->buf = buf;
  sb->cap = cap;
  sb->len = 0;
  sb->overflow = false;
  if (cap > 0) {
    sb->buf[0] = '\0';
  }
}

static void sb_printf(strbuf_t *sb, const char *fmt, ...) {
  if (sb->overflow || sb->cap == 0) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sb->cap - sb->len) {
    sb->overflow = true;
    sb->len = sb->cap - 1U;
    sb->buf[sb->len] = '\0';
    return;
  }
  sb->len += (size_t)n;
}

static void sb_json_string(strbuf_t *sb, const char *s) {
  sb_printf(sb, "\"");
  for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
    if (*p == '"') {
      sb_printf(sb, "\\\"");
    } else if (*p == '\\') {
      sb_printf(sb, "\\\\");
    } else if (*p == '\n') {
      sb_printf(sb, "\\n");
    } else if (*p == '\r') {
      sb_printf(sb, "\\r");
    } else if (*p == '\t') {
      sb_printf(sb, "\\t");
    } else if (*p < 0x20U) {
      sb_printf(sb, "\\u%04x", (unsigned)*p);
    } else {
      sb_printf(sb, "%c", (char)*p);
    }
  }
  sb_printf(sb, "\"");
}

static void sb_hex_bytes(strbuf_t *sb, const uint8_t *bytes, size_t count) {
  sb_printf(sb, "\"");
  for (size_t i = 0; i < count; ++i) {
    sb_printf(sb, "%02x", (unsigned)bytes[i]);
  }
  sb_printf(sb, "\"");
}

/* Minimal flat-object JSON parser for monitor requests. */
static const char *skip_ws(const char *p) {
  while (*p != '\0' && isspace((unsigned char)*p)) {
    ++p;
  }
  return p;
}

static const char *skip_json_string(const char *p) {
  if (*p != '"') {
    return NULL;
  }
  ++p;
  while (*p != '\0') {
    if (*p == '\\') {
      ++p;
      if (*p == '\0') {
        return NULL;
      }
      ++p;
    } else if (*p == '"') {
      return p + 1;
    } else {
      ++p;
    }
  }
  return NULL;
}

static const char *skip_json_value(const char *p) {
  p = skip_ws(p);
  if (*p == '"') {
    return skip_json_string(p);
  }
  if (*p == '{' || *p == '[') {
    const char open_c = *p;
    const char close_c = (open_c == '{') ? '}' : ']';
    int depth = 1;
    ++p;
    while (*p != '\0' && depth > 0) {
      if (*p == '"') {
        p = skip_json_string(p);
        if (!p) {
          return NULL;
        }
        continue;
      }
      if (*p == open_c) {
        ++depth;
      } else if (*p == close_c) {
        --depth;
      }
      ++p;
    }
    return (depth == 0) ? p : NULL;
  }
  while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
         !isspace((unsigned char)*p)) {
    ++p;
  }
  return p;
}

static const char *json_find_field(const char *json, const char *key) {
  const char *p = skip_ws(json);
  if (*p != '{') {
    return NULL;
  }
  ++p;
  const size_t key_len = strlen(key);
  while (*p != '\0') {
    p = skip_ws(p);
    if (*p == '}') {
      return NULL;
    }
    if (*p != '"') {
      return NULL;
    }
    const char *kstart = p + 1;
    const char *kend = skip_json_string(p);
    if (!kend) {
      return NULL;
    }
    const size_t cur_len = (size_t)((kend - 1) - kstart);
    p = skip_ws(kend);
    if (*p != ':') {
      return NULL;
    }
    ++p;
    p = skip_ws(p);
    if (cur_len == key_len && memcmp(kstart, key, key_len) == 0) {
      return p;
    }
    p = skip_json_value(p);
    if (!p) {
      return NULL;
    }
    p = skip_ws(p);
    if (*p == ',') {
      ++p;
    } else if (*p == '}') {
      return NULL;
    } else {
      return NULL;
    }
  }
  return NULL;
}

static bool json_parse_string_at(const char *val, char *out, size_t out_cap) {
  if (!val || *val != '"' || out_cap == 0) {
    return false;
  }
  ++val;
  size_t len = 0;
  while (*val != '\0' && *val != '"') {
    char c = *val++;
    if (c == '\\') {
      if (*val == '\0') {
        return false;
      }
      switch (*val++) {
        case '"':
          c = '"';
          break;
        case '\\':
          c = '\\';
          break;
        case '/':
          c = '/';
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        default:
          return false;
      }
    }
    if (len + 1U >= out_cap) {
      return false;
    }
    out[len++] = c;
  }
  if (*val != '"') {
    return false;
  }
  out[len] = '\0';
  return true;
}

static bool json_get_string(const char *json, const char *key, char *out,
                            size_t out_cap) {
  const char *v = json_find_field(json, key);
  return json_parse_string_at(v, out, out_cap);
}

static bool parse_u64_str(const char *s, uint64_t *out) {
  if (!s || *s == '\0') {
    return false;
  }
  while (isspace((unsigned char)*s)) {
    ++s;
  }
  if (*s == '-' || *s == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  const unsigned long long parsed = strtoull(s, &end, 0);
  if (errno != 0 || !end || end == s) {
    return false;
  }
  while (isspace((unsigned char)*end)) {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }
  *out = (uint64_t)parsed;
  return true;
}

static bool json_parse_u64_at(const char *val, uint64_t *out) {
  if (!val) {
    return false;
  }
  if (*val == '"') {
    char tmp[64];
    if (!json_parse_string_at(val, tmp, sizeof(tmp))) {
      return false;
    }
    return parse_u64_str(tmp, out);
  }
  if (*val == '-') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  const unsigned long long parsed = strtoull(val, &end, 10);
  if (errno != 0 || !end || end == val) {
    return false;
  }
  if (*end != '\0' && *end != ',' && *end != '}' &&
      !isspace((unsigned char)*end)) {
    return false;
  }
  *out = (uint64_t)parsed;
  return true;
}

static bool json_parse_bool_at(const char *val, bool *out) {
  if (!val) {
    return false;
  }
  if (strncmp(val, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (strncmp(val, "false", 5) == 0) {
    *out = false;
    return true;
  }
  if (*val == '"') {
    char tmp[32];
    if (!json_parse_string_at(val, tmp, sizeof(tmp))) {
      return false;
    }
    if (!strcmp(tmp, "true") || !strcmp(tmp, "True") || !strcmp(tmp, "1")) {
      *out = true;
      return true;
    }
    if (!strcmp(tmp, "false") || !strcmp(tmp, "False") || !strcmp(tmp, "0")) {
      *out = false;
      return true;
    }
    return false;
  }
  uint64_t num = 0;
  if (json_parse_u64_at(val, &num) && (num == 0U || num == 1U)) {
    *out = (num != 0U);
    return true;
  }
  return false;
}

/* ---- Diagnostics / Stack Watermark ------------------------------------- */

static uint16_t read_sp(const avr_t *avr) {
  return (uint16_t)(avr->data[R_SPL] | ((uint16_t)avr->data[R_SPH] << 8));
}

static void reset_diagnostics(void) {
  memset(&g_diag, 0, sizeof(g_diag));
  g_diag.min_sp = 0xFFFFU;
  g_crash_logged = false;
}

static bool is_min_sp_valid(const avr_t *avr) {
  return g_diag.min_sp != 0xFFFFU && g_diag.min_sp <= avr->ramend;
}

static void sample_sp(const avr_t *avr) {
  const uint16_t sp = read_sp(avr);
  if (sp == 0U || sp > avr->ramend || sp >= g_diag.min_sp) {
    return;
  }
  g_diag.min_sp = sp;
  g_diag.min_sp_pc = avr->pc;
  g_diag.min_sp_stack_len = 0;
  for (uint32_t addr = (uint32_t)sp + 1U;
       addr <= avr->ramend &&
       g_diag.min_sp_stack_len < PEAK_STACK_SNAPSHOT_BYTES;
       ++addr) {
    g_diag.min_sp_stack[g_diag.min_sp_stack_len++] = avr->data[addr];
  }
}

static void capture_crash_snapshot(const avr_t *avr) {
  if (g_diag.crashed) {
    return;
  }
  g_diag.crashed = true;
  g_diag.crash_pc = avr->pc;
  g_diag.crash_sp = read_sp(avr);
  g_diag.crash_sreg = avr->data[R_SREG];
  g_diag.crash_cycle = avr->cycle;
  g_diag.crash_stack_len = 0;
  for (uint32_t addr = (uint32_t)g_diag.crash_sp + 1U;
       addr <= avr->ramend && g_diag.crash_stack_len < CRASH_STACK_DUMP_BYTES;
       ++addr) {
    g_diag.crash_stack[g_diag.crash_stack_len++] = avr->data[addr];
  }
}

static void append_stack_stats_json(strbuf_t *sb, const avr_t *avr) {
  const bool valid = is_min_sp_valid(avr);
  sb_printf(sb, "{");
  sb_printf(sb, "\"min_sp_valid\":%s,", valid ? "true" : "false");
  if (valid) {
    sb_printf(sb, "\"min_sp\":%u,", (unsigned)g_diag.min_sp);
  } else {
    sb_printf(sb, "\"min_sp\":null,");
  }
  sb_printf(sb, "\"ramend\":%u,", (unsigned)avr->ramend);
  sb_printf(sb, "\"peak_pc\":%lu,", (unsigned long)g_diag.min_sp_pc);
  sb_printf(sb, "\"address_size\":%u,", (unsigned)avr->address_size);
  sb_printf(sb, "\"peak_stack_hex\":");
  sb_hex_bytes(sb, g_diag.min_sp_stack, g_diag.min_sp_stack_len);
  sb_printf(sb, ",\"crashed\":%s,", g_diag.crashed ? "true" : "false");
  sb_printf(sb, "\"crash_pc\":%lu,", (unsigned long)g_diag.crash_pc);
  sb_printf(sb, "\"crash_sp\":%u,", (unsigned)g_diag.crash_sp);
  sb_printf(sb, "\"crash_sreg\":%u,", (unsigned)g_diag.crash_sreg);
  sb_printf(sb, "\"crash_cycle\":%llu,",
            (unsigned long long)g_diag.crash_cycle);
  sb_printf(sb, "\"crash_stack_hex\":");
  sb_hex_bytes(sb, g_diag.crash_stack, g_diag.crash_stack_len);
  sb_printf(sb, "}");
}

static void write_diagnostics_file(const char *path, const avr_t *avr) {
  if (!path) {
    return;
  }
  char buf[4096];
  strbuf_t sb;
  sb_init(&sb, buf, sizeof(buf));
  append_stack_stats_json(&sb, avr);
  sb_printf(&sb, "\n");
  if (!sb.overflow) {
    (void)write_file_atomic(path, sb.buf, sb.len);
  }
}

/* ---- Socket helpers ----------------------------------------------------- */

static int set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_loopback_listener(uint16_t requested_port,
                                    uint16_t *actual_port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  const int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (set_nonblocking(fd) != 0) {
    close(fd);
    return -1;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(requested_port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 8) != 0) {
    close(fd);
    return -1;
  }
  socklen_t alen = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
    close(fd);
    return -1;
  }
  *actual_port = ntohs(addr.sin_port);
  return fd;
}

static int accept_nonblocking_client(int listen_fd) {
  const int fd = accept(listen_fd, NULL, NULL);
  if (fd < 0) {
    return -1;
  }
  (void)set_nonblocking(fd);
  const int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

/* ---- Multi-UART channel handling ---------------------------------------- */

static size_t queue_used(size_t head, size_t tail) {
  return (head + UART_QUEUE_CAPACITY - tail) % UART_QUEUE_CAPACITY;
}

static void uart_tx_cb(struct avr_irq_t *irq, uint32_t value, void *opaque) {
  (void)irq;
  uart_channel_t *ch = (uart_channel_t *)opaque;
  if (!ch) {
    return;
  }
  const size_t next = (ch->tx_head + 1U) % UART_QUEUE_CAPACITY;
  if (next == ch->tx_tail) {
    fprintf(stderr, "UART %s TX queue overflow\n", ch->name);
    g_failure = 1;
    g_stop = 1;
    return;
  }
  ch->tx_queue[ch->tx_head] = (uint8_t)value;
  ch->tx_head = next;
  ++ch->tx_bytes;
}

static void uart_xon_cb(struct avr_irq_t *irq, uint32_t value, void *opaque) {
  (void)irq;
  uart_channel_t *ch = (uart_channel_t *)opaque;
  if (ch) {
    ch->xon = (value != 0U);
  }
}

static void uart_xoff_cb(struct avr_irq_t *irq, uint32_t value, void *opaque) {
  (void)irq;
  uart_channel_t *ch = (uart_channel_t *)opaque;
  if (ch) {
    ch->xon = (value == 0U);
  }
}

static int uart_active_fd(const uart_channel_t *ch) {
  return (ch->backend == CHAN_TYPE_PTY) ? ch->pty_master : ch->tcp_client_fd;
}

static void uart_flush_tx(uart_channel_t *ch) {
  while (ch->tx_tail != ch->tx_head) {
    const int fd = uart_active_fd(ch);
    if (fd < 0) {
      break;
    }
    const size_t available = queue_used(ch->tx_head, ch->tx_tail);
    size_t contiguous = UART_QUEUE_CAPACITY - ch->tx_tail;
    if (contiguous > available) {
      contiguous = available;
    }
    const ssize_t written = write(fd, &ch->tx_queue[ch->tx_tail], contiguous);
    if (written > 0) {
      ch->tx_tail = (ch->tx_tail + (size_t)written) % UART_QUEUE_CAPACITY;
    } else if (written < 0 &&
               (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      break;
    } else if (written < 0 && ch->backend == CHAN_TYPE_PTY && errno == EIO) {
      break;
    } else if (written < 0 && ch->backend == CHAN_TYPE_TCP &&
               (errno == EPIPE || errno == ECONNRESET)) {
      close(ch->tcp_client_fd);
      ch->tcp_client_fd = -1;
      break;
    } else if (written < 0) {
      fprintf(stderr, "UART %s write failed: %s\n", ch->name, strerror(errno));
      g_failure = 1;
      g_stop = 1;
      break;
    } else {
      break;
    }
  }
}

static void uart_flush_all_tx(void) {
  for (size_t i = 0; i < g_uart_count; ++i) {
    uart_flush_tx(&g_uarts[i]);
  }
}

static void uart_read_host(uart_channel_t *ch) {
  const int fd = uart_active_fd(ch);
  if (fd < 0) {
    return;
  }
  uint8_t buf[256];
  for (;;) {
    const ssize_t count = read(fd, buf, sizeof(buf));
    if (count > 0) {
      for (ssize_t i = 0; i < count; ++i) {
        const size_t next = (ch->rx_head + 1U) % UART_QUEUE_CAPACITY;
        if (next != ch->rx_tail) {
          ch->rx_queue[ch->rx_head] = buf[i];
          ch->rx_head = next;
          ++ch->rx_bytes;
        } else {
          ++ch->rx_dropped;
          if (ch->rx_dropped == 1U || (ch->rx_dropped % 1024U) == 0U) {
            fprintf(stderr, "UART %s RX queue overflow (dropped=%llu)\n",
                    ch->name, (unsigned long long)ch->rx_dropped);
          }
        }
      }
      if ((size_t)count < sizeof(buf)) {
        break;
      }
    } else if (count == 0) {
      if (ch->backend == CHAN_TYPE_TCP) {
        close(ch->tcp_client_fd);
        ch->tcp_client_fd = -1;
      }
      break;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
               (ch->backend == CHAN_TYPE_PTY && errno == EIO)) {
      break;
    } else if (ch->backend == CHAN_TYPE_TCP &&
               (errno == ECONNRESET || errno == EPIPE)) {
      close(ch->tcp_client_fd);
      ch->tcp_client_fd = -1;
      break;
    } else {
      fprintf(stderr, "UART %s read failed: %s\n", ch->name, strerror(errno));
      g_failure = 1;
      g_stop = 1;
      break;
    }
  }
}

static void uart_disable_stdio(avr_t *avr, char uart_id) {
  uint32_t uart_flags = 0;
  avr_ioctl(avr, AVR_IOCTL_UART_GET_FLAGS(uart_id), &uart_flags);
  uart_flags &= ~AVR_UART_FLAG_STDIO;
  avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS(uart_id), &uart_flags);
}

static int init_uart_channel(avr_t *avr, const char *mcu,
                             const uart_spec_t *spec, uart_channel_t *ch) {
  memset(ch, 0, sizeof(*ch));
  snprintf(ch->name, sizeof(ch->name), "%s", spec->name);
  ch->uart_id = spec->uart_id;
  ch->backend = spec->backend;
  ch->pty_master = -1;
  ch->pty_slave = -1;
  ch->tcp_listen_fd = -1;
  ch->tcp_client_fd = -1;
  ch->xon = true;

  avr_irq_t *tx =
      avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(ch->uart_id), UART_IRQ_OUTPUT);
  avr_irq_t *rx =
      avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(ch->uart_id), UART_IRQ_INPUT);
  avr_irq_t *xon =
      avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(ch->uart_id), UART_IRQ_OUT_XON);
  avr_irq_t *xoff =
      avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(ch->uart_id), UART_IRQ_OUT_XOFF);
  if (!tx || !rx) {
    fprintf(stderr, "UART%c is unavailable on MCU %s\n", ch->uart_id, mcu);
    return -1;
  }

  if (ch->backend == CHAN_TYPE_PTY) {
    if (openpty(&ch->pty_master, &ch->pty_slave, ch->pty_path, NULL, NULL) !=
        0) {
      fprintf(stderr, "openpty failed for %s: %s\n", ch->name, strerror(errno));
      return -1;
    }
    struct termios settings;
    if (tcgetattr(ch->pty_slave, &settings) != 0) {
      fprintf(stderr, "tcgetattr failed for %s: %s\n", ch->name,
              strerror(errno));
      close(ch->pty_slave);
      close(ch->pty_master);
      ch->pty_slave = -1;
      ch->pty_master = -1;
      return -1;
    }
    cfmakeraw(&settings);
    if (tcsetattr(ch->pty_slave, TCSANOW, &settings) != 0) {
      fprintf(stderr, "tcsetattr failed for %s: %s\n", ch->name,
              strerror(errno));
      close(ch->pty_slave);
      close(ch->pty_master);
      ch->pty_slave = -1;
      ch->pty_master = -1;
      return -1;
    }
    (void)set_nonblocking(ch->pty_master);
  } else {
    ch->tcp_listen_fd = create_loopback_listener(0, &ch->tcp_port);
    if (ch->tcp_listen_fd < 0) {
      fprintf(stderr, "TCP listener failed for %s: %s\n", ch->name,
              strerror(errno));
      return -1;
    }
  }

  uart_disable_stdio(avr, ch->uart_id);
  ch->rx_irq = rx;
  avr_irq_register_notify(tx, uart_tx_cb, ch);
  if (xon) {
    avr_irq_register_notify(xon, uart_xon_cb, ch);
  }
  if (xoff) {
    avr_irq_register_notify(xoff, uart_xoff_cb, ch);
  }
  return 0;
}

static void close_uart_channels(void) {
  for (size_t i = 0; i < g_uart_count; ++i) {
    uart_channel_t *ch = &g_uarts[i];
    if (ch->pty_slave >= 0) {
      close(ch->pty_slave);
      ch->pty_slave = -1;
    }
    if (ch->pty_master >= 0) {
      close(ch->pty_master);
      ch->pty_master = -1;
    }
    if (ch->tcp_client_fd >= 0) {
      close(ch->tcp_client_fd);
      ch->tcp_client_fd = -1;
    }
    if (ch->tcp_listen_fd >= 0) {
      close(ch->tcp_listen_fd);
      ch->tcp_listen_fd = -1;
    }
  }
}

static void drain_all_rx_queues(avr_t *avr) {
  for (size_t u = 0; u < g_uart_count; ++u) {
    uart_channel_t *ch = &g_uarts[u];
    for (size_t b = 0; b < DRAIN_BATCH_BYTES; ++b) {
      if (!ch->xon || ch->rx_tail == ch->rx_head || !ch->rx_irq) {
        break;
      }
      const uint8_t byte = ch->rx_queue[ch->rx_tail];
      ch->rx_tail = (ch->rx_tail + 1U) % UART_QUEUE_CAPACITY;
      avr_raise_irq(ch->rx_irq, byte);
      for (uint32_t c = 0; c < g_cycles_per_uart_byte; ++c) {
        if (avr->state == cpu_Done || avr->state == cpu_Crashed ||
            avr->state == cpu_Stopped || g_stop || g_paused) {
          return;
        }
        (void)avr_run(avr);
        sample_sp(avr);
      }
      uart_flush_all_tx();
    }
  }
}

/* ---- MCU Reset & State helpers ------------------------------------------ */

static const char *avr_state_name(int state) {
  switch (state) {
    case cpu_Limbo:
      return "limbo";
    case cpu_Stopped:
      return "stopped";
    case cpu_Running:
      return "running";
    case cpu_Sleeping:
      return "sleeping";
    case cpu_Step:
      return "step";
    case cpu_StepDone:
      return "step_done";
    case cpu_Done:
      return "done";
    case cpu_Crashed:
      return "crashed";
    default:
      return "unknown";
  }
}

static void reset_mcu(avr_t *avr) {
  for (size_t i = 0; i < g_uart_count; ++i) {
    g_uarts[i].tx_head = 0;
    g_uarts[i].tx_tail = 0;
    g_uarts[i].rx_head = 0;
    g_uarts[i].rx_tail = 0;
    g_uarts[i].xon = true;
  }
  reset_diagnostics();
  if (g_has_reset_pc) {
    avr->reset_pc = g_reset_pc;
  }
  avr_reset(avr);
  avr->vcc = g_vcc_mv;
  avr->avcc = g_avcc_mv;
  avr->aref = g_aref_mv;
  for (size_t i = 0; i < g_uart_count; ++i) {
    uart_disable_stdio(avr, g_uarts[i].uart_id);
  }
  if (g_paused) {
    avr->state = cpu_Stopped;
  }
}

/* ---- Property Tree Implementation --------------------------------------- */

static void send_monitor_line(monitor_client_t *client, const char *data,
                              size_t len) {
  if (client->fd < 0) {
    return;
  }
  size_t off = 0;
  int retries = 0;
  while (off < len) {
    const ssize_t w = write(client->fd, data + off, len - off);
    if (w > 0) {
      off += (size_t)w;
    } else if (w < 0 && errno == EINTR) {
      continue;
    } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
               retries < 50) {
      ++retries;
      usleep(1000);
    } else {
      close(client->fd);
      client->fd = -1;
      client->rx_len = 0;
      return;
    }
  }
}

static void monitor_reply_error(strbuf_t *sb, const char *code,
                                const char *msg) {
  sb_printf(sb, "{\"ok\":false,\"error\":");
  sb_json_string(sb, code);
  sb_printf(sb, ",\"message\":");
  sb_json_string(sb, msg);
  sb_printf(sb, "}\n");
}

static bool has_gpio_port(avr_t *avr, char port_letter,
                          avr_ioport_state_t *out_state) {
  if (port_letter < 'A' || port_letter > 'L') {
    return false;
  }
  avr_ioport_state_t st;
  memset(&st, 0, sizeof(st));
  if (avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE(port_letter), &st) == 0) {
    if (out_state) {
      *out_state = st;
    }
    return true;
  }
  return false;
}

static bool parse_gpio_port_path(const char *path, char *out_port) {
  if (strncmp(path, "/gpio/port", 10) != 0) {
    return false;
  }
  if (path[10] == '\0' || path[11] != '\0') {
    return false;
  }
  const char c = (char)toupper((unsigned char)path[10]);
  if (c < 'A' || c > 'L') {
    return false;
  }
  *out_port = c;
  return true;
}

static uart_channel_t *find_uart_by_name(const char *name) {
  for (size_t i = 0; i < g_uart_count; ++i) {
    if (strcmp(g_uarts[i].name, name) == 0) {
      return &g_uarts[i];
    }
  }
  return NULL;
}

static bool has_adc_peripheral(avr_t *avr) {
  return avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_ADC0) != NULL;
}

static void handle_prop_list(avr_t *avr, const char *path, strbuf_t *sb) {
  if (strcmp(path, "/") == 0) {
    sb_printf(sb,
              "{\"ok\":true,\"result\":[\"cpu\",\"stack\",\"sram\",\"io\","
              "\"flash\",\"eeprom\",\"adc\",\"gpio\",\"uart\"]}\n");
    return;
  }
  if (strcmp(path, "/cpu") == 0) {
    sb_printf(sb,
              "{\"ok\":true,\"result\":[\"mcu\",\"core\",\"frequency\",\"pc\",\"sp\","
              "\"sreg\",\"cycle\",\"state\",\"paused\",\"ramend\",\"flashend\","
              "\"e2end\",\"address_size\",\"vcc\",\"avcc\",\"aref\","
              "\"log_level\",\"trace\"");
    for (unsigned r = 0; r < 32U; ++r) {
      sb_printf(sb, ",\"r%u\"", r);
    }
    sb_printf(sb, "]}\n");
    return;
  }
  if (strcmp(path, "/stack") == 0) {
    sb_printf(sb,
              "{\"ok\":true,\"result\":[\"min_sp\",\"min_sp_valid\",\"ramend\","
              "\"peak_pc\",\"address_size\",\"peak_stack_hex\",\"crashed\","
              "\"crash_pc\",\"crash_sp\",\"crash_sreg\",\"crash_cycle\","
              "\"crash_stack_hex\"]}\n");
    return;
  }
  if (strcmp(path, "/sram") == 0) {
    sb_printf(sb, "{\"ok\":true,\"result\":[\"size\",\"ramend\"]}\n");
    return;
  }
  if (strcmp(path, "/io") == 0) {
    sb_printf(sb, "{\"ok\":true,\"result\":[\"ioend\"]}\n");
    return;
  }
  if (strcmp(path, "/flash") == 0) {
    sb_printf(sb, "{\"ok\":true,\"result\":[\"size\",\"flashend\"]}\n");
    return;
  }
  if (strcmp(path, "/eeprom") == 0) {
    sb_printf(sb, "{\"ok\":true,\"result\":[\"size\",\"e2end\"]}\n");
    return;
  }
  if (strcmp(path, "/adc") == 0) {
    if (!has_adc_peripheral(avr)) {
      monitor_reply_error(sb, "invalid_path", "ADC peripheral unavailable");
      return;
    }
    sb_printf(sb, "{\"ok\":true,\"result\":[");
    for (unsigned i = 0; i < 16U; ++i) {
      sb_printf(sb, "%s\"adc%u\"", i == 0U ? "" : ",", i);
    }
    sb_printf(sb, ",\"temp\"]}\n");
    return;
  }
  if (strcmp(path, "/gpio") == 0) {
    sb_printf(sb, "{\"ok\":true,\"result\":[");
    bool first = true;
    for (char c = 'A'; c <= 'L'; ++c) {
      if (has_gpio_port(avr, c, NULL)) {
        sb_printf(sb, "%s\"port%c\"", first ? "" : ",", c);
        first = false;
      }
    }
    sb_printf(sb, "]}\n");
    return;
  }
  char port_letter = '\0';
  if (parse_gpio_port_path(path, &port_letter) &&
      has_gpio_port(avr, port_letter, NULL)) {
    sb_printf(sb,
              "{\"ok\":true,\"result\":[\"port\",\"ddr\",\"pin\","
              "\"pin0\",\"pin1\",\"pin2\",\"pin3\",\"pin4\",\"pin5\","
              "\"pin6\",\"pin7\"]}\n");
    return;
  }
  if (strcmp(path, "/uart") == 0) {
    sb_printf(sb, "{\"ok\":true,\"result\":[");
    for (size_t i = 0; i < g_uart_count; ++i) {
      if (i > 0) {
        sb_printf(sb, ",");
      }
      sb_json_string(sb, g_uarts[i].name);
    }
    sb_printf(sb, "]}\n");
    return;
  }
  if (strncmp(path, "/uart/", 6) == 0 && find_uart_by_name(path + 6) != NULL) {
    sb_printf(sb,
              "{\"ok\":true,\"result\":[\"id\",\"type\",\"xon\",\"tx_bytes\","
              "\"rx_bytes\",\"rx_dropped\",\"tx_queued\",\"rx_queued\"]}\n");
    return;
  }
  monitor_reply_error(sb, "invalid_path", "Unknown property path");
}

static bool parse_reg_index(const char *prop, unsigned *out_reg) {
  if (prop[0] != 'r' || !isdigit((unsigned char)prop[1])) {
    return false;
  }
  uint64_t idx = 0;
  if (!parse_u64_str(prop + 1, &idx) || idx >= 32U) {
    return false;
  }
  *out_reg = (unsigned)idx;
  return true;
}

static bool parse_pin_index(const char *prop, int *out_pin) {
  if (strncmp(prop, "pin", 3) != 0 || prop[3] < '0' || prop[3] > '7' ||
      prop[4] != '\0') {
    return false;
  }
  *out_pin = prop[3] - '0';
  return true;
}

static bool parse_adc_index(const char *prop, int *out_idx) {
  if (strcmp(prop, "temp") == 0) {
    *out_idx = ADC_IRQ_TEMP;
    return true;
  }
  if (strncmp(prop, "adc", 3) != 0 || !isdigit((unsigned char)prop[3])) {
    return false;
  }
  uint64_t idx = 0;
  if (!parse_u64_str(prop + 3, &idx) || idx >= 16U) {
    return false;
  }
  *out_idx = (int)idx;
  return true;
}

static void handle_prop_get(avr_t *avr, const char *path, const char *prop,
                            strbuf_t *sb) {
  if (strcmp(path, "/cpu") == 0) {
    unsigned reg = 0;
    if (strcmp(prop, "mcu") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":");
      sb_json_string(sb, g_mcu_name);
      sb_printf(sb, "}\n");
    } else if (strcmp(prop, "core") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":");
      sb_json_string(sb, avr->mmcu ? avr->mmcu : "");
      sb_printf(sb, "}\n");
    } else if (strcmp(prop, "frequency") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->frequency);
    } else if (strcmp(prop, "pc") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%lu}\n", (unsigned long)avr->pc);
    } else if (strcmp(prop, "sp") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)read_sp(avr));
    } else if (strcmp(prop, "sreg") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n",
                (unsigned)avr->data[R_SREG]);
    } else if (strcmp(prop, "cycle") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%llu}\n",
                (unsigned long long)avr->cycle);
    } else if (strcmp(prop, "state") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":");
      sb_json_string(sb, avr_state_name(avr->state));
      sb_printf(sb, "}\n");
    } else if (strcmp(prop, "paused") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%s}\n",
                (g_paused || avr->state == cpu_Stopped) ? "true" : "false");
    } else if (strcmp(prop, "ramend") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)avr->ramend);
    } else if (strcmp(prop, "flashend") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->flashend);
    } else if (strcmp(prop, "e2end") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->e2end);
    } else if (strcmp(prop, "address_size") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n",
                (unsigned)avr->address_size);
    } else if (strcmp(prop, "vcc") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->vcc);
    } else if (strcmp(prop, "avcc") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->avcc);
    } else if (strcmp(prop, "aref") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->aref);
    } else if (strcmp(prop, "log_level") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)avr->log);
    } else if (strcmp(prop, "trace") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%s}\n",
                avr->trace ? "true" : "false");
    } else if (parse_reg_index(prop, &reg)) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)avr->data[reg]);
    } else {
      monitor_reply_error(sb, "invalid_property", "Unknown /cpu property");
    }
    return;
  }

  if (strcmp(path, "/stack") == 0) {
    if (strcmp(prop, "min_sp") == 0) {
      if (is_min_sp_valid(avr)) {
        sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)g_diag.min_sp);
      } else {
        sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
      }
    } else if (strcmp(prop, "min_sp_valid") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%s}\n",
                is_min_sp_valid(avr) ? "true" : "false");
    } else if (strcmp(prop, "ramend") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)avr->ramend);
    } else if (strcmp(prop, "peak_pc") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%lu}\n",
                (unsigned long)g_diag.min_sp_pc);
    } else if (strcmp(prop, "address_size") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n",
                (unsigned)avr->address_size);
    } else if (strcmp(prop, "peak_stack_hex") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":");
      sb_hex_bytes(sb, g_diag.min_sp_stack, g_diag.min_sp_stack_len);
      sb_printf(sb, "}\n");
    } else if (strcmp(prop, "crashed") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%s}\n",
                g_diag.crashed ? "true" : "false");
    } else if (strcmp(prop, "crash_pc") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%lu}\n",
                (unsigned long)g_diag.crash_pc);
    } else if (strcmp(prop, "crash_sp") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)g_diag.crash_sp);
    } else if (strcmp(prop, "crash_sreg") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n",
                (unsigned)g_diag.crash_sreg);
    } else if (strcmp(prop, "crash_cycle") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%llu}\n",
                (unsigned long long)g_diag.crash_cycle);
    } else if (strcmp(prop, "crash_stack_hex") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":");
      sb_hex_bytes(sb, g_diag.crash_stack, g_diag.crash_stack_len);
      sb_printf(sb, "}\n");
    } else {
      monitor_reply_error(sb, "invalid_property", "Unknown /stack property");
    }
    return;
  }

  if (strcmp(path, "/sram") == 0) {
    if (strcmp(prop, "size") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n",
                (unsigned)avr->ramend + 1U);
      return;
    }
    if (strcmp(prop, "ramend") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)avr->ramend);
      return;
    }
    uint64_t addr = 0;
    if (parse_u64_str(prop, &addr) && addr <= (uint64_t)avr->ramend) {
      const uint8_t val = avr_core_watch_read(avr, (uint16_t)addr);
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)val);
      return;
    }
    monitor_reply_error(sb, "invalid_property", "Invalid /sram address");
    return;
  }

  if (strcmp(path, "/io") == 0) {
    if (strcmp(prop, "ioend") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)avr->ioend);
      return;
    }
    uint64_t addr = 0;
    if (parse_u64_str(prop, &addr)) {
      const uint64_t data_addr = AVR_IO_TO_DATA(addr);
      if (data_addr <= (uint64_t)avr->ramend &&
          (avr->ioend == 0U || data_addr <= (uint64_t)avr->ioend)) {
        const uint8_t val = avr_core_watch_read(avr, (uint16_t)data_addr);
        sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)val);
        return;
      }
    }
    monitor_reply_error(sb, "invalid_property", "Invalid /io address");
    return;
  }

  if (strcmp(path, "/flash") == 0) {
    if (strcmp(prop, "size") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->flashend + 1U);
      return;
    }
    if (strcmp(prop, "flashend") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->flashend);
      return;
    }
    uint64_t addr = 0;
    if (parse_u64_str(prop, &addr) && addr <= (uint64_t)avr->flashend) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n",
                (unsigned)avr->flash[addr]);
      return;
    }
    monitor_reply_error(sb, "invalid_property", "Invalid /flash address");
    return;
  }

  if (strcmp(path, "/eeprom") == 0) {
    if (strcmp(prop, "size") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->e2end + 1U);
      return;
    }
    if (strcmp(prop, "e2end") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", avr->e2end);
      return;
    }
    uint64_t addr = 0;
    if (parse_u64_str(prop, &addr) && addr <= (uint64_t)avr->e2end) {
      // Note: libsimavr's avr_eeprom_ioctl() populates desc.ee when NULL and
      // performs the copy, while returning -1; validate success via desc.ee.
      avr_eeprom_desc_t probe = {
          .ee = NULL, .offset = (uint16_t)addr, .size = 1U};
      (void)avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &probe);
      if (probe.ee != NULL) {
        sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)probe.ee[0]);
        return;
      }
      monitor_reply_error(sb, "runtime_error", "EEPROM peripheral unavailable");
      return;
    }
    monitor_reply_error(sb, "invalid_property", "Invalid /eeprom address");
    return;
  }

  if (strcmp(path, "/adc") == 0) {
    if (!has_adc_peripheral(avr)) {
      monitor_reply_error(sb, "invalid_path", "ADC peripheral unavailable");
      return;
    }
    int idx = 0;
    if (!parse_adc_index(prop, &idx) || idx < 0 || idx >= ADC_CHANNEL_COUNT) {
      monitor_reply_error(sb, "invalid_property", "Unknown /adc property");
      return;
    }
    sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", g_adc_mv[idx]);
    return;
  }

  char port_letter = '\0';
  avr_ioport_state_t st;
  if (parse_gpio_port_path(path, &port_letter) &&
      has_gpio_port(avr, port_letter, &st)) {
    int pin_idx = 0;
    if (strcmp(prop, "port") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)st.port);
    } else if (strcmp(prop, "ddr") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)st.ddr);
    } else if (strcmp(prop, "pin") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n", (unsigned)st.pin);
    } else if (parse_pin_index(prop, &pin_idx)) {
      sb_printf(sb, "{\"ok\":true,\"result\":%u}\n",
                (unsigned)((st.pin >> pin_idx) & 1U));
    } else {
      monitor_reply_error(sb, "invalid_property", "Unknown GPIO port property");
    }
    return;
  }

  if (strncmp(path, "/uart/", 6) == 0) {
    uart_channel_t *ch = find_uart_by_name(path + 6);
    if (!ch) {
      monitor_reply_error(sb, "invalid_path", "Unknown UART channel");
      return;
    }
    if (strcmp(prop, "id") == 0) {
      char id_str[2] = {ch->uart_id, '\0'};
      sb_printf(sb, "{\"ok\":true,\"result\":");
      sb_json_string(sb, id_str);
      sb_printf(sb, "}\n");
    } else if (strcmp(prop, "type") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":");
      sb_json_string(sb, ch->backend == CHAN_TYPE_PTY ? "pty" : "tcp");
      sb_printf(sb, "}\n");
    } else if (strcmp(prop, "xon") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%s}\n",
                ch->xon ? "true" : "false");
    } else if (strcmp(prop, "tx_bytes") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%llu}\n",
                (unsigned long long)ch->tx_bytes);
    } else if (strcmp(prop, "rx_bytes") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%llu}\n",
                (unsigned long long)ch->rx_bytes);
    } else if (strcmp(prop, "rx_dropped") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%llu}\n",
                (unsigned long long)ch->rx_dropped);
    } else if (strcmp(prop, "tx_queued") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%zu}\n",
                queue_used(ch->tx_head, ch->tx_tail));
    } else if (strcmp(prop, "rx_queued") == 0) {
      sb_printf(sb, "{\"ok\":true,\"result\":%zu}\n",
                queue_used(ch->rx_head, ch->rx_tail));
    } else {
      monitor_reply_error(sb, "invalid_property", "Unknown /uart property");
    }
    return;
  }

  monitor_reply_error(sb, "invalid_path", "Unknown property path");
}

static void handle_prop_set(avr_t *avr, const char *path, const char *prop,
                            const char *raw_val, strbuf_t *sb) {
  if (!raw_val) {
    monitor_reply_error(sb, "invalid_value", "Missing property value");
    return;
  }

  if (strcmp(path, "/cpu") == 0) {
    unsigned reg = 0;
    uint64_t num = 0;
    bool flag = false;
    if (strcmp(prop, "pc") == 0) {
      if (!json_parse_u64_at(raw_val, &num) || num > avr->flashend) {
        monitor_reply_error(sb, "invalid_value", "PC out of flash range");
        return;
      }
      avr->pc = (avr_flashaddr_t)num;
    } else if (strcmp(prop, "sreg") == 0) {
      if (!json_parse_u64_at(raw_val, &num) || num > 0xFFU) {
        monitor_reply_error(sb, "invalid_value", "SREG must be 0..255");
        return;
      }
      avr->data[R_SREG] = (uint8_t)num;
      for (int bit = 0; bit < 8; ++bit) {
        avr->sreg[bit] = (uint8_t)((num >> bit) & 1U);
      }
    } else if (strcmp(prop, "paused") == 0) {
      if (!json_parse_bool_at(raw_val, &flag)) {
        monitor_reply_error(sb, "invalid_value", "paused must be boolean");
        return;
      }
      g_paused = flag;
      if (flag) {
        avr->state = cpu_Stopped;
      } else if (avr->state == cpu_Stopped) {
        avr->state = cpu_Running;
      }
    } else if (strcmp(prop, "vcc") == 0) {
      if (!json_parse_u64_at(raw_val, &num) || num > UINT32_MAX) {
        monitor_reply_error(sb, "invalid_value", "Invalid vcc mV");
        return;
      }
      g_vcc_mv = (uint32_t)num;
      avr->vcc = g_vcc_mv;
    } else if (strcmp(prop, "avcc") == 0) {
      if (!json_parse_u64_at(raw_val, &num) || num > UINT32_MAX) {
        monitor_reply_error(sb, "invalid_value", "Invalid avcc mV");
        return;
      }
      g_avcc_mv = (uint32_t)num;
      avr->avcc = g_avcc_mv;
    } else if (strcmp(prop, "aref") == 0) {
      if (!json_parse_u64_at(raw_val, &num) || num > UINT32_MAX) {
        monitor_reply_error(sb, "invalid_value", "Invalid aref mV");
        return;
      }
      g_aref_mv = (uint32_t)num;
      avr->aref = g_aref_mv;
    } else if (strcmp(prop, "log_level") == 0) {
      if (!json_parse_u64_at(raw_val, &num) || num > LOG_DEBUG) {
        monitor_reply_error(sb, "invalid_value", "log_level must be 0..5");
        return;
      }
      g_log_level = (int)num;
      avr->log = (uint8_t)(num & 0x0FU);
    } else if (strcmp(prop, "trace") == 0) {
      if (!json_parse_bool_at(raw_val, &flag)) {
        monitor_reply_error(sb, "invalid_value", "trace must be boolean");
        return;
      }
      avr->trace = flag ? 1U : 0U;
    } else if (parse_reg_index(prop, &reg)) {
      if (!json_parse_u64_at(raw_val, &num) || num > 0xFFU) {
        monitor_reply_error(sb, "invalid_value", "Register value must be 0..255");
        return;
      }
      avr->data[reg] = (uint8_t)num;
    } else if (strcmp(prop, "mcu") == 0 || strcmp(prop, "core") == 0 ||
               strcmp(prop, "frequency") == 0 ||
               strcmp(prop, "sp") == 0 || strcmp(prop, "cycle") == 0 ||
               strcmp(prop, "state") == 0 || strcmp(prop, "ramend") == 0 ||
               strcmp(prop, "flashend") == 0 || strcmp(prop, "e2end") == 0 ||
               strcmp(prop, "address_size") == 0) {
      monitor_reply_error(sb, "read_only", "Property is read-only");
      return;
    } else {
      monitor_reply_error(sb, "invalid_property", "Unknown /cpu property");
      return;
    }
    sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
    return;
  }

  if (strcmp(path, "/stack") == 0) {
    bool flag = false;
    if (strcmp(prop, "reset") == 0 && json_parse_bool_at(raw_val, &flag)) {
      if (flag) {
        reset_diagnostics();
      }
      sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
      return;
    }
    monitor_reply_error(sb, "invalid_property",
                        "Only /stack.reset can be written");
    return;
  }

  if (strcmp(path, "/sram") == 0) {
    uint64_t addr = 0;
    uint64_t val = 0;
    if (!parse_u64_str(prop, &addr) || addr > (uint64_t)avr->ramend) {
      monitor_reply_error(sb, "invalid_property", "Invalid /sram address");
      return;
    }
    if (!json_parse_u64_at(raw_val, &val) || val > 0xFFU) {
      monitor_reply_error(sb, "invalid_value", "SRAM byte must be 0..255");
      return;
    }
    avr_core_watch_write(avr, (uint16_t)addr, (uint8_t)val);
    sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
    return;
  }

  if (strcmp(path, "/io") == 0) {
    uint64_t addr = 0;
    uint64_t val = 0;
    if (!parse_u64_str(prop, &addr)) {
      monitor_reply_error(sb, "invalid_property", "Invalid /io address");
      return;
    }
    const uint64_t data_addr = AVR_IO_TO_DATA(addr);
    if (data_addr > (uint64_t)avr->ramend ||
        (avr->ioend != 0U && data_addr > (uint64_t)avr->ioend)) {
      monitor_reply_error(sb, "invalid_property", "IO address out of range");
      return;
    }
    if (!json_parse_u64_at(raw_val, &val) || val > 0xFFU) {
      monitor_reply_error(sb, "invalid_value", "IO byte must be 0..255");
      return;
    }
    avr_core_watch_write(avr, (uint16_t)data_addr, (uint8_t)val);
    sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
    return;
  }

  if (strcmp(path, "/flash") == 0) {
    monitor_reply_error(sb, "read_only", "/flash is read-only via monitor");
    return;
  }

  if (strcmp(path, "/eeprom") == 0) {
    uint64_t addr = 0;
    uint64_t val = 0;
    if (!parse_u64_str(prop, &addr) || addr > (uint64_t)avr->e2end) {
      monitor_reply_error(sb, "invalid_property", "Invalid /eeprom address");
      return;
    }
    if (!json_parse_u64_at(raw_val, &val) || val > 0xFFU) {
      monitor_reply_error(sb, "invalid_value", "EEPROM byte must be 0..255");
      return;
    }
    avr_eeprom_desc_t probe = {
        .ee = NULL, .offset = (uint16_t)addr, .size = 1U};
    (void)avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &probe);
    if (probe.ee == NULL) {
      monitor_reply_error(sb, "runtime_error", "EEPROM peripheral unavailable");
      return;
    }
    uint8_t byte = (uint8_t)val;
    avr_eeprom_desc_t desc = {
        .ee = &byte, .offset = (uint16_t)addr, .size = 1U};
    (void)avr_ioctl(avr, AVR_IOCTL_EEPROM_SET, &desc);
    sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
    return;
  }

  if (strcmp(path, "/adc") == 0) {
    if (!has_adc_peripheral(avr)) {
      monitor_reply_error(sb, "invalid_path", "ADC peripheral unavailable");
      return;
    }
    int idx = 0;
    uint64_t mv = 0;
    if (!parse_adc_index(prop, &idx) || idx < 0 || idx >= ADC_CHANNEL_COUNT) {
      monitor_reply_error(sb, "invalid_property", "Unknown /adc property");
      return;
    }
    if (!json_parse_u64_at(raw_val, &mv) || mv > 10000U) {
      monitor_reply_error(sb, "invalid_value",
                          "ADC millivolts must be 0..10000");
      return;
    }
    avr_irq_t *irq = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, idx);
    if (!irq) {
      monitor_reply_error(sb, "invalid_property", "ADC channel IRQ unavailable");
      return;
    }
    g_adc_mv[idx] = (uint32_t)mv;
    avr_raise_irq(irq, (uint32_t)mv);
    sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
    return;
  }

  char port_letter = '\0';
  if (parse_gpio_port_path(path, &port_letter) &&
      has_gpio_port(avr, port_letter, NULL)) {
    int pin_idx = 0;
    uint64_t val = 0;
    if (parse_pin_index(prop, &pin_idx)) {
      if (!json_parse_u64_at(raw_val, &val) || val > 1U) {
        monitor_reply_error(sb, "invalid_value", "Pin level must be 0 or 1");
        return;
      }
      avr_irq_t *irq = avr_io_getirq(
          avr, AVR_IOCTL_IOPORT_GETIRQ(port_letter), IOPORT_IRQ_PIN0 + pin_idx);
      if (!irq) {
        monitor_reply_error(sb, "runtime_error", "GPIO pin IRQ unavailable");
        return;
      }
      avr_raise_irq(irq, (uint32_t)val);
      sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
      return;
    }
    if (strcmp(prop, "pin") == 0) {
      if (!json_parse_u64_at(raw_val, &val) || val > 0xFFU) {
        monitor_reply_error(sb, "invalid_value", "Port mask must be 0..255");
        return;
      }
      avr_ioport_external_t ext;
      memset(&ext, 0, sizeof(ext));
      ext.name = (uint8_t)((unsigned char)port_letter & 0x7FU);
      ext.mask = 0xFFU;
      ext.value = (uint8_t)val;
      (void)avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL(port_letter), &ext);
      // IOPORT_IRQ_PIN0..PIN7 are the input IRQs handled by avr_ioport.
      for (int bit = 0; bit < 8; ++bit) {
        avr_irq_t *irq = avr_io_getirq(
            avr, AVR_IOCTL_IOPORT_GETIRQ(port_letter), IOPORT_IRQ_PIN0 + bit);
        if (irq) {
          avr_raise_irq(irq, (uint32_t)((val >> bit) & 1U));
        }
      }
      sb_printf(sb, "{\"ok\":true,\"result\":null}\n");
      return;
    }
    if (strcmp(prop, "port") == 0 || strcmp(prop, "ddr") == 0) {
      monitor_reply_error(sb, "read_only",
                          "PORT/DDR are driven by firmware; write pin or pin0..7");
      return;
    }
    monitor_reply_error(sb, "invalid_property", "Unknown GPIO port property");
    return;
  }

  if (strncmp(path, "/uart/", 6) == 0 && find_uart_by_name(path + 6) != NULL) {
    monitor_reply_error(sb, "read_only", "/uart properties are read-only");
    return;
  }

  monitor_reply_error(sb, "invalid_path", "Unknown property path");
}

/* ---- Monitor command dispatcher ----------------------------------------- */

static void handle_monitor_line(avr_t *avr, monitor_client_t *client,
                                const char *line) {
  char resp[MONITOR_RESP_MAX];
  strbuf_t sb;
  sb_init(&sb, resp, sizeof(resp));

  char cmd[64];
  if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) {
    monitor_reply_error(&sb, "bad_request", "Missing string field 'cmd'");
    send_monitor_line(client, sb.buf, sb.len);
    return;
  }

  if (strcmp(cmd, "status") == 0) {
    sb_printf(&sb,
              "{\"ok\":true,\"result\":{\"state\":");
    sb_json_string(&sb, avr_state_name(avr->state));
    sb_printf(&sb,
              ",\"paused\":%s,\"crashed\":%s,\"pc\":%lu,\"sp\":%u,"
              "\"cycle\":%llu}}\n",
              (g_paused || avr->state == cpu_Stopped) ? "true" : "false",
              g_diag.crashed ? "true" : "false", (unsigned long)avr->pc,
              (unsigned)read_sp(avr), (unsigned long long)avr->cycle);
  } else if (strcmp(cmd, "reset") == 0) {
    reset_mcu(avr);
    sb_printf(&sb, "{\"ok\":true,\"result\":{\"state\":");
    sb_json_string(&sb, avr_state_name(avr->state));
    sb_printf(&sb, "}}\n");
  } else if (strcmp(cmd, "pause") == 0) {
    g_paused = true;
    avr->state = cpu_Stopped;
    sb_printf(&sb,
              "{\"ok\":true,\"result\":{\"paused\":true,\"pc\":%lu,"
              "\"cycle\":%llu}}\n",
              (unsigned long)avr->pc, (unsigned long long)avr->cycle);
  } else if (strcmp(cmd, "cont") == 0) {
    g_paused = false;
    if (avr->state == cpu_Stopped) {
      avr->state = cpu_Running;
    }
    sb_printf(&sb, "{\"ok\":true,\"result\":{\"paused\":false}}\n");
  } else if (strcmp(cmd, "step") == 0) {
    uint64_t steps = 1;
    const char *cycles_val = json_find_field(line, "cycles");
    if (cycles_val) {
      if (!json_parse_u64_at(cycles_val, &steps) || steps == 0U ||
          steps > 1000000U) {
        monitor_reply_error(&sb, "invalid_value", "cycles must be 1..1000000");
        send_monitor_line(client, sb.buf, sb.len);
        return;
      }
    }
    const bool was_paused = g_paused;
    g_paused = false;
    if (avr->state == cpu_Stopped) {
      avr->state = cpu_Running;
    }
    for (uint64_t i = 0; i < steps; ++i) {
      if (avr->state == cpu_Done || avr->state == cpu_Crashed || g_stop) {
        break;
      }
      (void)avr_run(avr);
      sample_sp(avr);
    }
    if (avr->state == cpu_Crashed) {
      capture_crash_snapshot(avr);
    } else if (was_paused || avr->state != cpu_Done) {
      g_paused = true;
      avr->state = cpu_Stopped;
    }
    uart_flush_all_tx();
    sb_printf(&sb,
              "{\"ok\":true,\"result\":{\"pc\":%lu,\"sp\":%u,\"cycle\":%llu,"
              "\"state\":",
              (unsigned long)avr->pc, (unsigned)read_sp(avr),
              (unsigned long long)avr->cycle);
    sb_json_string(&sb, avr_state_name(avr->state));
    sb_printf(&sb, "}}\n");
  } else if (strcmp(cmd, "stack_stats") == 0) {
    sb_printf(&sb, "{\"ok\":true,\"result\":");
    append_stack_stats_json(&sb, avr);
    sb_printf(&sb, "}\n");
  } else if (strcmp(cmd, "prop_list") == 0) {
    char path[128];
    if (!json_get_string(line, "path", path, sizeof(path))) {
      monitor_reply_error(&sb, "bad_request", "Missing string field 'path'");
    } else {
      handle_prop_list(avr, path, &sb);
    }
  } else if (strcmp(cmd, "prop_get") == 0) {
    char path[128];
    char prop[64];
    if (!json_get_string(line, "path", path, sizeof(path)) ||
        !json_get_string(line, "property", prop, sizeof(prop))) {
      monitor_reply_error(&sb, "bad_request",
                          "Missing 'path' or 'property' string field");
    } else {
      handle_prop_get(avr, path, prop, &sb);
    }
  } else if (strcmp(cmd, "prop_set") == 0) {
    char path[128];
    char prop[64];
    const char *raw_val = json_find_field(line, "value");
    if (!json_get_string(line, "path", path, sizeof(path)) ||
        !json_get_string(line, "property", prop, sizeof(prop))) {
      monitor_reply_error(&sb, "bad_request",
                          "Missing 'path' or 'property' string field");
    } else {
      handle_prop_set(avr, path, prop, raw_val, &sb);
    }
  } else if (strcmp(cmd, "quit") == 0) {
    g_stop = 1;
    sb_printf(&sb, "{\"ok\":true,\"result\":\"stopping\"}\n");
  } else {
    monitor_reply_error(&sb, "bad_request", "Unknown monitor command");
  }

  send_monitor_line(client, sb.buf, sb.len);
}

static void monitor_read_client(avr_t *avr, monitor_client_t *client) {
  if (client->fd < 0) {
    return;
  }
  for (;;) {
    if (client->rx_len + 1U >= sizeof(client->rx_buf)) {
      char err_buf[256];
      strbuf_t sb;
      sb_init(&sb, err_buf, sizeof(err_buf));
      monitor_reply_error(&sb, "bad_request", "Monitor request line too long");
      send_monitor_line(client, sb.buf, sb.len);
      client->rx_len = 0;
      return;
    }
    const ssize_t n = read(client->fd, client->rx_buf + client->rx_len,
                           sizeof(client->rx_buf) - 1U - client->rx_len);
    if (n > 0) {
      client->rx_len += (size_t)n;
      client->rx_buf[client->rx_len] = '\0';
      char *nl = NULL;
      while (client->fd >= 0 &&
             (nl = memchr(client->rx_buf, '\n', client->rx_len)) != NULL) {
        *nl = '\0';
        const size_t line_len = (size_t)(nl - client->rx_buf);
        if (line_len > 0 && client->rx_buf[line_len - 1U] == '\r') {
          client->rx_buf[line_len - 1U] = '\0';
        }
        if (client->rx_buf[0] != '\0') {
          handle_monitor_line(avr, client, client->rx_buf);
        }
        const size_t consumed = line_len + 1U;
        const size_t remaining = client->rx_len - consumed;
        if (remaining > 0) {
          memmove(client->rx_buf, client->rx_buf + consumed, remaining);
        }
        client->rx_len = remaining;
        client->rx_buf[client->rx_len] = '\0';
      }
      if (client->fd < 0) {
        return;
      }
    } else if (n == 0) {
      close(client->fd);
      client->fd = -1;
      client->rx_len = 0;
      return;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return;
    } else {
      close(client->fd);
      client->fd = -1;
      client->rx_len = 0;
      return;
    }
  }
}

static void init_monitor_struct(void) {
  g_monitor.listen_fd = -1;
  g_monitor.port = 0;
  for (size_t i = 0; i < MAX_MONITOR_CLIENTS; ++i) {
    g_monitor.clients[i].fd = -1;
    g_monitor.clients[i].rx_len = 0;
  }
}

static void close_monitor_server(void) {
  for (size_t i = 0; i < MAX_MONITOR_CLIENTS; ++i) {
    if (g_monitor.clients[i].fd >= 0) {
      close(g_monitor.clients[i].fd);
      g_monitor.clients[i].fd = -1;
      g_monitor.clients[i].rx_len = 0;
    }
  }
  if (g_monitor.listen_fd >= 0) {
    close(g_monitor.listen_fd);
    g_monitor.listen_fd = -1;
  }
}

static bool is_unprogrammed_reset_vector(const avr_t *avr) {
  return avr->pc == 0U && avr->flashend >= 1U && avr->flash[0] == 0xFFU &&
         avr->flash[1] == 0xFFU;
}

static bool is_firmware_sleep_halt(const avr_t *avr) {
  if (avr->sreg[S_I] != 0U || avr->pc < 2U || avr->pc - 2U + 1U > avr->flashend) {
    return false;
  }
  const uint16_t prev_opcode = (uint16_t)(avr->flash[avr->pc - 2U] |
                                          ((uint16_t)avr->flash[avr->pc - 1U] << 8));
  return prev_opcode == 0x9588U;  // AVR SLEEP instruction with interrupts off
}

/* ---- Unified non-blocking poll() loop ----------------------------------- */

static void poll_host_io(avr_t *avr, int timeout_ms) {
  struct pollfd fds[1 + MAX_MONITOR_CLIENTS + MAX_UART_CHANNELS * 2];
  enum { KIND_MON_LISTEN, KIND_MON_CLIENT, KIND_UART_LISTEN, KIND_UART_IO };
  struct {
    int kind;
    size_t index;
  } meta[1 + MAX_MONITOR_CLIENTS + MAX_UART_CHANNELS * 2];
  nfds_t nfds = 0;

  if (g_monitor.listen_fd >= 0) {
    fds[nfds].fd = g_monitor.listen_fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;
    meta[nfds].kind = KIND_MON_LISTEN;
    meta[nfds].index = 0;
    ++nfds;
  }
  for (size_t i = 0; i < MAX_MONITOR_CLIENTS; ++i) {
    if (g_monitor.clients[i].fd >= 0) {
      fds[nfds].fd = g_monitor.clients[i].fd;
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      meta[nfds].kind = KIND_MON_CLIENT;
      meta[nfds].index = i;
      ++nfds;
    }
  }
  for (size_t i = 0; i < g_uart_count; ++i) {
    uart_channel_t *ch = &g_uarts[i];
    if (ch->backend == CHAN_TYPE_TCP && ch->tcp_listen_fd >= 0) {
      fds[nfds].fd = ch->tcp_listen_fd;
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      meta[nfds].kind = KIND_UART_LISTEN;
      meta[nfds].index = i;
      ++nfds;
    }
    const int io_fd = uart_active_fd(ch);
    if (io_fd >= 0) {
      fds[nfds].fd = io_fd;
      fds[nfds].events = POLLIN;
      if (ch->tx_tail != ch->tx_head) {
        fds[nfds].events |= POLLOUT;
      }
      fds[nfds].revents = 0;
      meta[nfds].kind = KIND_UART_IO;
      meta[nfds].index = i;
      ++nfds;
    }
  }

  if (nfds == 0) {
    if (timeout_ms > 0) {
      usleep((useconds_t)(timeout_ms * 1000));
    }
    return;
  }

  const int ready = poll(fds, nfds, timeout_ms);
  if (ready <= 0) {
    return;
  }

  for (nfds_t k = 0; k < nfds; ++k) {
    const short rev = fds[k].revents;
    if (rev == 0) {
      continue;
    }
    if (meta[k].kind == KIND_MON_LISTEN && (rev & POLLIN)) {
      const int cfd = accept_nonblocking_client(g_monitor.listen_fd);
      if (cfd >= 0) {
        bool placed = false;
        for (size_t c = 0; c < MAX_MONITOR_CLIENTS; ++c) {
          if (g_monitor.clients[c].fd < 0) {
            g_monitor.clients[c].fd = cfd;
            g_monitor.clients[c].rx_len = 0;
            placed = true;
            break;
          }
        }
        if (!placed) {
          // Evict oldest client slot 0 if saturated.
          close(g_monitor.clients[0].fd);
          g_monitor.clients[0].fd = cfd;
          g_monitor.clients[0].rx_len = 0;
        }
      }
    } else if (meta[k].kind == KIND_MON_CLIENT) {
      monitor_client_t *cl = &g_monitor.clients[meta[k].index];
      if (rev & (POLLIN | POLLHUP | POLLERR)) {
        monitor_read_client(avr, cl);
      }
    } else if (meta[k].kind == KIND_UART_LISTEN && (rev & POLLIN)) {
      uart_channel_t *ch = &g_uarts[meta[k].index];
      const int cfd = accept_nonblocking_client(ch->tcp_listen_fd);
      if (cfd >= 0) {
        if (ch->tcp_client_fd >= 0) {
          close(ch->tcp_client_fd);
        }
        ch->tcp_client_fd = cfd;
        uart_flush_tx(ch);
      }
    } else if (meta[k].kind == KIND_UART_IO) {
      uart_channel_t *ch = &g_uarts[meta[k].index];
      if (rev & (POLLIN | POLLHUP | POLLERR)) {
        uart_read_host(ch);
      }
      if (rev & POLLOUT) {
        uart_flush_tx(ch);
      }
    }
  }
}

/*
 * Responsive sleep callback so SLEEP_MODE_IDLE never stalls the monitor TCP
 * server, GDB RSP polling, or multi-UART I/O.
 */
static void simavr_responsive_sleep(avr_t *avr, avr_cycle_count_t how_long) {
  (void)how_long;
  if (g_stop || g_paused) {
    return;
  }
  if (avr->gdb) {
    (void)avr_gdb_processor(avr, 0);
  }
  poll_host_io(avr, 1);
  // If host bytes arrived during sleep, inject one byte immediately so RX IRQ
  // wakes the sleeping MCU without waiting for a full batch.
  for (size_t u = 0; u < g_uart_count; ++u) {
    uart_channel_t *ch = &g_uarts[u];
    if (ch->xon && ch->rx_tail != ch->rx_head && ch->rx_irq) {
      const uint8_t byte = ch->rx_queue[ch->rx_tail];
      ch->rx_tail = (ch->rx_tail + 1U) % UART_QUEUE_CAPACITY;
      avr_raise_irq(ch->rx_irq, byte);
    }
  }
}

/* ---- Channels metadata JSON --------------------------------------------- */

static int write_channels_file(const char *path, const char *mcu,
                               uint32_t frequency) {
  char buf[4096];
  strbuf_t sb;
  sb_init(&sb, buf, sizeof(buf));

  sb_printf(&sb, "{\"pid\":%ld,\"mcu\":", (long)getpid());
  sb_json_string(&sb, mcu);
  sb_printf(&sb, ",\"frequency\":%u", frequency);
  if (g_monitor.listen_fd >= 0) {
    sb_printf(&sb,
              ",\"monitor\":{\"type\":\"tcp\",\"host\":\"127.0.0.1\","
              "\"port\":%u}",
              (unsigned)g_monitor.port);
  }
  if (g_gdb_port > 0) {
    sb_printf(&sb,
              ",\"gdb\":{\"type\":\"tcp\",\"host\":\"127.0.0.1\","
              "\"port\":%d}",
              g_gdb_port);
  }
  sb_printf(&sb, ",\"uarts\":{");
  for (size_t i = 0; i < g_uart_count; ++i) {
    const uart_channel_t *ch = &g_uarts[i];
    if (i > 0) {
      sb_printf(&sb, ",");
    }
    sb_json_string(&sb, ch->name);
    char id_str[2] = {ch->uart_id, '\0'};
    if (ch->backend == CHAN_TYPE_PTY) {
      sb_printf(&sb, ":{\"id\":");
      sb_json_string(&sb, id_str);
      sb_printf(&sb, ",\"type\":\"pty\",\"path\":");
      sb_json_string(&sb, ch->pty_path);
      sb_printf(&sb, "}");
    } else {
      sb_printf(&sb, ":{\"id\":");
      sb_json_string(&sb, id_str);
      sb_printf(&sb,
                ",\"type\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":%u}",
                (unsigned)ch->tcp_port);
    }
  }
  sb_printf(&sb, "}}\n");
  if (sb.overflow) {
    fprintf(stderr, "channels.json buffer overflow\n");
    return -1;
  }
  return write_file_atomic(path, sb.buf, sb.len);
}

/* ---- Firmware / EEPROM loading helpers ---------------------------------- */

static bool ends_with_ci(const char *s, const char *suffix) {
  const size_t slen = strlen(s);
  const size_t suflen = strlen(suffix);
  if (slen < suflen) {
    return false;
  }
  const char *tail = s + (slen - suflen);
  for (size_t i = 0; i < suflen; ++i) {
    if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) {
      return false;
    }
  }
  return true;
}

static int load_firmware_image(avr_t *avr, const char *path,
                               uint32_t frequency) {
  if (ends_with_ci(path, ".hex") || ends_with_ci(path, ".ihex")) {
    ihex_chunk_p chunks = NULL;
    const int count = read_ihex_chunks(path, &chunks);
    if (count <= 0 || !chunks) {
      fprintf(stderr, "Unable to read Intel HEX firmware: %s\n", path);
      return -1;
    }
    for (int i = 0; i < count; ++i) {
      if (chunks[i].baseaddr >= AVR_SEGMENT_OFFSET_EEPROM) {
        const uint32_t ee_off =
            chunks[i].baseaddr - AVR_SEGMENT_OFFSET_EEPROM;
        avr_eeprom_desc_t desc = {
            .ee = chunks[i].data,
            .offset = (uint16_t)ee_off,
            .size = chunks[i].size,
        };
        (void)avr_ioctl(avr, AVR_IOCTL_EEPROM_SET, &desc);
      } else {
        avr_loadcode(avr, chunks[i].data, chunks[i].size, chunks[i].baseaddr);
      }
    }
    free_ihex_chunks(chunks);
    avr->frequency = frequency;
    return 0;
  }

  elf_firmware_t firmware;
  memset(&firmware, 0, sizeof(firmware));
  if (elf_read_firmware(path, &firmware) != 0) {
    fprintf(stderr, "Unable to read firmware ELF: %s\n", path);
    return -1;
  }
  firmware.frequency = frequency;
  avr_load_firmware(avr, &firmware);
  return 0;
}

static int load_eeprom_image(avr_t *avr, const char *path) {
  if (ends_with_ci(path, ".hex") || ends_with_ci(path, ".ihex")) {
    uint32_t size = 0;
    uint32_t start = 0;
    uint8_t *data = read_ihex_file(path, &size, &start);
    if (!data || size == 0) {
      fprintf(stderr, "Unable to read EEPROM HEX file: %s\n", path);
      free(data);
      return -1;
    }
    if (start >= AVR_SEGMENT_OFFSET_EEPROM) {
      start -= AVR_SEGMENT_OFFSET_EEPROM;
    }
    avr_eeprom_desc_t probe = {
        .ee = NULL, .offset = (uint16_t)start, .size = size};
    (void)avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &probe);
    if (probe.ee == NULL) {
      free(data);
      return -1;
    }
    avr_eeprom_desc_t desc = {
        .ee = data, .offset = (uint16_t)start, .size = size};
    (void)avr_ioctl(avr, AVR_IOCTL_EEPROM_SET, &desc);
    free(data);
    return 0;
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "Unable to open EEPROM image %s: %s\n", path,
            strerror(errno));
    return -1;
  }
  const size_t max_len = (size_t)avr->e2end + 1U;
  uint8_t *buf = malloc(max_len);
  if (!buf) {
    fclose(f);
    return -1;
  }
  const size_t n = fread(buf, 1, max_len, f);
  fclose(f);
  if (n > 0) {
    avr_eeprom_desc_t probe = {
        .ee = NULL, .offset = 0, .size = (uint32_t)n};
    (void)avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &probe);
    if (probe.ee == NULL) {
      free(buf);
      return -1;
    }
    avr_eeprom_desc_t desc = {.ee = buf, .offset = 0, .size = (uint32_t)n};
    (void)avr_ioctl(avr, AVR_IOCTL_EEPROM_SET, &desc);
  }
  free(buf);
  return 0;
}

/* ---- Argument parsing --------------------------------------------------- */

static bool parse_uart_spec(const char *arg, uart_spec_t *out) {
  // Format: <name>:<id>:<pty|tcp>
  const char *c1 = strchr(arg, ':');
  if (!c1 || c1 == arg) {
    return false;
  }
  const char *c2 = strchr(c1 + 1, ':');
  if (!c2 || (c2 - (c1 + 1)) != 1) {
    return false;
  }
  const size_t name_len = (size_t)(c1 - arg);
  if (name_len == 0 || name_len >= sizeof(out->name)) {
    return false;
  }
  const char uid = c1[1];
  if (uid < '0' || uid > '3') {
    return false;
  }
  const char *type_str = c2 + 1;
  uart_backend_t backend;
  if (strcmp(type_str, "pty") == 0) {
    backend = CHAN_TYPE_PTY;
  } else if (strcmp(type_str, "tcp") == 0) {
    backend = CHAN_TYPE_TCP;
  } else {
    return false;
  }
  memcpy(out->name, arg, name_len);
  out->name[name_len] = '\0';
  out->uart_id = uid;
  out->backend = backend;
  return true;
}

static bool parse_log_level_arg(const char *val, int *out_level) {
  if (strcmp(val, "none") == 0) {
    *out_level = LOG_NONE;
  } else if (strcmp(val, "output") == 0) {
    *out_level = LOG_OUTPUT;
  } else if (strcmp(val, "error") == 0) {
    *out_level = LOG_ERROR;
  } else if (strcmp(val, "warning") == 0) {
    *out_level = LOG_WARNING;
  } else if (strcmp(val, "trace") == 0) {
    *out_level = LOG_TRACE;
  } else if (strcmp(val, "debug") == 0) {
    *out_level = LOG_DEBUG;
  } else {
    return false;
  }
  return true;
}

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s --mcu MCU --frequency HZ "
          "--uart-channel NAME:ID:pty|tcp [--uart-channel ...] "
          "[--firmware FILE] [--channels-file PATH] "
          "[--diagnostics-file PATH] [--monitor-port PORT] "
          "[--gdb-port PORT] [--pause] [--log-level LEVEL] "
          "[--cycles-per-uart-byte N] [--vcc MV] [--avcc MV] "
          "[--aref MV] [--eeprom FILE] [--reset-pc ADDR]\n",
          program);
}

int main(int argc, char **argv) {
  const char *firmware_path = NULL;
  const char *eeprom_path = NULL;
  const char *channels_file_path = NULL;
  const char *diagnostics_file_path = NULL;
  const char *mcu = NULL;
  uint32_t frequency = 0;
  int monitor_port = -1;
  bool monitor_port_explicit = false;
  uart_spec_t uart_specs[MAX_UART_CHANNELS];
  size_t uart_spec_count = 0;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--firmware") && i + 1 < argc) {
      firmware_path = argv[++i];
    } else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc) {
      eeprom_path = argv[++i];
    } else if (!strcmp(argv[i], "--mcu") && i + 1 < argc) {
      mcu = argv[++i];
    } else if (!strcmp(argv[i], "--frequency") && i + 1 < argc) {
      uint64_t parsed = 0;
      if (!parse_u64_str(argv[++i], &parsed) || parsed == 0 ||
          parsed > UINT32_MAX) {
        fprintf(stderr, "Invalid frequency\n");
        return 2;
      }
      frequency = (uint32_t)parsed;
    } else if (!strcmp(argv[i], "--channels-file") && i + 1 < argc) {
      channels_file_path = argv[++i];
    } else if (!strcmp(argv[i], "--diagnostics-file") && i + 1 < argc) {
      diagnostics_file_path = argv[++i];
    } else if (!strcmp(argv[i], "--pause")) {
      g_paused = true;
    } else if (!strcmp(argv[i], "--uart-channel") && i + 1 < argc) {
      if (uart_spec_count >= MAX_UART_CHANNELS) {
        fprintf(stderr, "Too many --uart-channel entries (max %d)\n",
                MAX_UART_CHANNELS);
        return 2;
      }
      if (!parse_uart_spec(argv[++i], &uart_specs[uart_spec_count])) {
        fprintf(stderr, "Invalid --uart-channel (expected NAME:ID:pty|tcp)\n");
        return 2;
      }
      ++uart_spec_count;
    } else if (!strcmp(argv[i], "--monitor-port") && i + 1 < argc) {
      const char *val = argv[++i];
      if (strcmp(val, "-1") == 0) {
        monitor_port = -1;
      } else {
        uint64_t parsed = 0;
        if (!parse_u64_str(val, &parsed) || parsed > 65535U) {
          fprintf(stderr, "Invalid --monitor-port\n");
          return 2;
        }
        monitor_port = (int)parsed;
      }
      monitor_port_explicit = true;
    } else if (!strcmp(argv[i], "--gdb-port") && i + 1 < argc) {
      uint64_t parsed = 0;
      if (!parse_u64_str(argv[++i], &parsed) || parsed == 0 ||
          parsed > 65535U) {
        fprintf(stderr, "Invalid --gdb-port\n");
        return 2;
      }
      g_gdb_port = (int)parsed;
    } else if (!strcmp(argv[i], "--log-level") && i + 1 < argc) {
      if (!parse_log_level_arg(argv[++i], &g_log_level)) {
        fprintf(stderr, "Invalid --log-level\n");
        return 2;
      }
    } else if (!strcmp(argv[i], "--cycles-per-uart-byte") && i + 1 < argc) {
      uint64_t parsed = 0;
      if (!parse_u64_str(argv[++i], &parsed) || parsed == 0 ||
          parsed > 1000000U) {
        fprintf(stderr, "Invalid --cycles-per-uart-byte\n");
        return 2;
      }
      g_cycles_per_uart_byte = (uint32_t)parsed;
    } else if (!strcmp(argv[i], "--vcc") && i + 1 < argc) {
      uint64_t parsed = 0;
      if (!parse_u64_str(argv[++i], &parsed) || parsed > UINT32_MAX) {
        fprintf(stderr, "Invalid --vcc\n");
        return 2;
      }
      g_vcc_mv = (uint32_t)parsed;
    } else if (!strcmp(argv[i], "--avcc") && i + 1 < argc) {
      uint64_t parsed = 0;
      if (!parse_u64_str(argv[++i], &parsed) || parsed > UINT32_MAX) {
        fprintf(stderr, "Invalid --avcc\n");
        return 2;
      }
      g_avcc_mv = (uint32_t)parsed;
    } else if (!strcmp(argv[i], "--aref") && i + 1 < argc) {
      uint64_t parsed = 0;
      if (!parse_u64_str(argv[++i], &parsed) || parsed > UINT32_MAX) {
        fprintf(stderr, "Invalid --aref\n");
        return 2;
      }
      g_aref_mv = (uint32_t)parsed;
    } else if (!strcmp(argv[i], "--reset-pc") && i + 1 < argc) {
      uint64_t parsed = 0;
      if (!parse_u64_str(argv[++i], &parsed) || parsed > UINT32_MAX) {
        fprintf(stderr, "Invalid --reset-pc\n");
        return 2;
      }
      g_reset_pc = (avr_flashaddr_t)parsed;
      g_has_reset_pc = true;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if (!mcu || frequency == 0 || uart_spec_count == 0 ||
      (!firmware_path && !(g_paused && g_gdb_port > 0))) {
    usage(argv[0]);
    return 2;
  }

  // Validate unique UART names and IDs.
  for (size_t i = 0; i < uart_spec_count; ++i) {
    for (size_t j = i + 1U; j < uart_spec_count; ++j) {
      if (strcmp(uart_specs[i].name, uart_specs[j].name) == 0 ||
          uart_specs[i].uart_id == uart_specs[j].uart_id) {
        fprintf(stderr, "Duplicate UART channel name or ID\n");
        return 2;
      }
    }
  }

  if (!monitor_port_explicit && channels_file_path != NULL) {
    monitor_port = 0;
  }

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, stop_handler);
  signal(SIGTERM, stop_handler);

  reset_diagnostics();
  init_monitor_struct();
  avr_global_logger_set(&safe_simavr_logger);

  snprintf(g_mcu_name, sizeof(g_mcu_name), "%s", mcu);
  struct avr_t *avr = avr_make_mcu_by_name(mcu);
  if (!avr) {
    fprintf(stderr, "Unknown simavr MCU: %s\n", mcu);
    return 2;
  }
  avr->frequency = frequency;
  avr->vcc = g_vcc_mv;
  avr->avcc = g_avcc_mv;
  avr->aref = g_aref_mv;
  avr->log = (uint8_t)((unsigned)g_log_level & 0x0FU);
  if (avr_init(avr) != 0) {
    fprintf(stderr, "Unable to initialize MCU: %s\n", mcu);
    avr_terminate(avr);
    return 1;
  }
  avr->log = (uint8_t)((unsigned)g_log_level & 0x0FU);
  avr->vcc = g_vcc_mv;
  avr->avcc = g_avcc_mv;
  avr->aref = g_aref_mv;

  if (firmware_path) {
    if (load_firmware_image(avr, firmware_path, frequency) != 0) {
      avr_terminate(avr);
      return 1;
    }
  }
  if (eeprom_path) {
    if (load_eeprom_image(avr, eeprom_path) != 0) {
      avr_terminate(avr);
      return 1;
    }
  }
  if (g_has_reset_pc) {
    avr->reset_pc = g_reset_pc;
    avr->pc = g_reset_pc;
  }

  for (size_t i = 0; i < uart_spec_count; ++i) {
    if (init_uart_channel(avr, mcu, &uart_specs[i], &g_uarts[i]) != 0) {
      g_uart_count = i;
      close_uart_channels();
      avr_terminate(avr);
      return 1;
    }
    g_uart_count = i + 1U;
  }

  if (monitor_port >= 0) {
    g_monitor.listen_fd =
        create_loopback_listener((uint16_t)monitor_port, &g_monitor.port);
    if (g_monitor.listen_fd < 0) {
      fprintf(stderr, "Unable to open monitor socket on port %d: %s\n",
              monitor_port, strerror(errno));
      close_uart_channels();
      avr_terminate(avr);
      return 1;
    }
  }

  if (g_gdb_port > 0) {
    avr->gdb_port = g_gdb_port;
    if (avr_gdb_init(avr) != 0) {
      fprintf(stderr, "Unable to initialize simavr GDB server on port %d\n",
              g_gdb_port);
      close_monitor_server();
      close_uart_channels();
      avr_terminate(avr);
      return 1;
    }
  }

  // Override sleep AFTER avr_init and avr_gdb_init so SLEEP_MODE_IDLE remains
  // immediately responsive to monitor TCP, GDB RSP, and multi-UART traffic.
  avr->sleep = simavr_responsive_sleep;
  if (g_paused) {
    avr->state = cpu_Stopped;
  }

  if (channels_file_path) {
    if (write_channels_file(channels_file_path, mcu, frequency) != 0) {
      if (avr->gdb) {
        avr_deinit_gdb(avr);
      }
      close_monitor_server();
      close_uart_channels();
      avr_terminate(avr);
      return 1;
    }
  }

  while (!g_stop && avr->state != cpu_Done && avr->state != cpu_Crashed) {
    if (g_paused || avr->state == cpu_Stopped) {
      if (avr->gdb) {
        (void)avr_gdb_processor(avr, 0);
        // libsimavr's GDB 'D' (detach) handler sets cpu_Done; keep the
        // simulator alive so subsequent pw_emu/monitor/GDB sessions work.
        if (avr->state == cpu_Done) {
          avr->state = cpu_Stopped;
        } else if ((avr->state == cpu_Running || avr->state == cpu_Step) &&
                   is_unprogrammed_reset_vector(avr)) {
          // Do not execute erased 0xFF flash when a GDB client disconnects
          // before loading firmware.
          avr->state = cpu_Stopped;
        } else if (avr->state == cpu_Running || avr->state == cpu_Step) {
          g_paused = false;
          continue;
        }
        poll_host_io(avr, 1);
      } else {
        poll_host_io(avr, 10);
      }
      continue;
    }

    poll_host_io(avr, 0);
    if (g_stop || g_paused || avr->state == cpu_Stopped) {
      continue;
    }

    drain_all_rx_queues(avr);

    for (size_t step = 0; step < BATCH_INSTRUCTION_CYCLES; ++step) {
      if (avr->state == cpu_Done || avr->state == cpu_Crashed ||
          avr->state == cpu_Stopped || g_stop || g_paused) {
        break;
      }
      (void)avr_run(avr);
      if (avr->gdb && avr->state == cpu_Done && !is_firmware_sleep_halt(avr)) {
        avr->state = cpu_Running;
      }
      sample_sp(avr);
    }

    if (avr->state == cpu_Crashed) {
      capture_crash_snapshot(avr);
      break;
    }
    if (avr->state == cpu_Stopped) {
      if (g_crash_logged) {
        capture_crash_snapshot(avr);
      }
      g_paused = true;
    }

    drain_all_rx_queues(avr);
    uart_flush_all_tx();
  }

  if (avr->state == cpu_Crashed) {
    capture_crash_snapshot(avr);
  }

  // Drain pending UART bytes queued by final instructions before closing.
  for (unsigned attempt = 0; attempt < 2000; ++attempt) {
    bool any_pending = false;
    for (size_t i = 0; i < g_uart_count; ++i) {
      uart_channel_t *ch = &g_uarts[i];
      if (ch->tx_tail != ch->tx_head && uart_active_fd(ch) >= 0) {
        uart_flush_tx(ch);
        if (ch->tx_tail != ch->tx_head && uart_active_fd(ch) >= 0) {
          any_pending = true;
        }
      }
    }
    if (!any_pending) {
      break;
    }
    usleep(1000);
  }
  for (size_t i = 0; i < g_uart_count; ++i) {
    const size_t dropped = queue_used(g_uarts[i].tx_head, g_uarts[i].tx_tail);
    if (dropped > 0) {
      fprintf(stderr, "Warning: dropping %zu queued UART %s bytes on shutdown\n",
              dropped, g_uarts[i].name);
    }
  }

  write_diagnostics_file(diagnostics_file_path, avr);

  if (avr->state == cpu_Crashed || g_diag.crashed) {
    fprintf(stderr, "AVR simulation crashed\n");
    g_failure = 1;
  }

  if (avr->gdb) {
    avr_deinit_gdb(avr);
  }
  close_monitor_server();
  close_uart_channels();
  avr_terminate(avr);
  return g_failure ? 1 : 0;
}
