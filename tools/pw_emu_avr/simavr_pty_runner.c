/*
 * Host-only simavr runner exposing AVR USART0 through a POSIX PTY.
 * The firmware is unchanged; all emulation code runs on the host.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "avr_uart.h"
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_irq.h"

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reset = 0;
static volatile sig_atomic_t g_continue = 0;
static int g_failure = 0;
static int g_pty_master = -1;
static int g_pty_slave = -1;
static bool g_uart_xon = true;
// Host-side buffering absorbs UART bursts while the PTY consumer is scheduled.
static uint8_t g_tx_queue[65536];
static size_t g_tx_head = 0;
static size_t g_tx_tail = 0;
// Host-to-AVR RX FIFO with cycle-paced injection so AVR UART RX ISR never overruns.
static uint8_t g_rx_queue[65536];
static size_t g_rx_head = 0;
static size_t g_rx_tail = 0;

enum {
  DRAIN_BATCH_BYTES = 64,
  CYCLES_PER_UART_BYTE = 1500,
  BATCH_INSTRUCTION_CYCLES = 2048
};

static void stop_handler(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

static void reset_handler(int signal_number) {
  (void)signal_number;
  g_reset = 1;
}

static void continue_handler(int signal_number) {
  (void)signal_number;
  g_continue = 1;
}

static void uart_tx(struct avr_irq_t *irq, uint32_t value, void *opaque) {
  (void)irq;
  (void)opaque;
  const size_t next = (g_tx_head + 1U) % sizeof(g_tx_queue);
  if (next == g_tx_tail) {
    fprintf(stderr, "UART TX queue overflow\n");
    g_failure = 1;
    g_stop = 1;
    return;
  }
  g_tx_queue[g_tx_head] = (uint8_t)value;
  g_tx_head = next;
}

static void uart_xon(struct avr_irq_t *irq, uint32_t value, void *opaque) {
  (void)irq;
  (void)opaque;
  g_uart_xon = (value != 0U);
}

static void uart_xoff(struct avr_irq_t *irq, uint32_t value, void *opaque) {
  (void)irq;
  (void)opaque;
  g_uart_xon = (value == 0U);
}

static void flush_tx_queue(void) {
  while (g_tx_tail != g_tx_head && g_pty_master >= 0) {
    size_t available =
        (g_tx_head + sizeof(g_tx_queue) - g_tx_tail) % sizeof(g_tx_queue);
    size_t contiguous = sizeof(g_tx_queue) - g_tx_tail;
    if (contiguous > available) contiguous = available;
    const ssize_t written =
        write(g_pty_master, &g_tx_queue[g_tx_tail], contiguous);
    if (written > 0) {
      g_tx_tail = (g_tx_tail + (size_t)written) % sizeof(g_tx_queue);
    } else if (written < 0 &&
               (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
                errno == EIO)) {
      break;
    } else if (written < 0) {
      fprintf(stderr, "PTY write failed: %s\n", strerror(errno));
      g_failure = 1;
      g_stop = 1;
      break;
    } else {
      break;
    }
  }
}

static void drain_rx_queue(struct avr_t *avr, avr_irq_t *rx) {
  for (size_t b = 0; b < DRAIN_BATCH_BYTES; ++b) {
    if (!g_uart_xon || g_rx_tail == g_rx_head) {
      break;
    }
    const uint8_t byte = g_rx_queue[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1U) % sizeof(g_rx_queue);
    avr_raise_irq(rx, byte);
    for (size_t c = 0; c < CYCLES_PER_UART_BYTE; ++c) {
      if (avr->state == cpu_Done || avr->state == cpu_Crashed || g_stop ||
          g_reset) {
        return;
      }
      (void)avr_run(avr);
    }
    flush_tx_queue();
  }
}

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s --firmware FILE [--mcu atmega328p] "
                  "[--frequency HZ] [--uart 0] [--pty-file PATH]\n", program);
}

int main(int argc, char **argv) {
  const char *firmware_path = NULL;
  const char *pty_file_path = NULL;
  const char *mcu = "atmega328p";
  uint32_t frequency = 16000000;
  char uart = '0';
  bool paused = false;

  if (argc >= 2 && argv[1][0] != '-') {
    firmware_path = argv[1];
    if (argc >= 3) mcu = argv[2];
    if (argc >= 4) {
      char *end = NULL;
      unsigned long parsed = strtoul(argv[3], &end, 10);
      if (!end || *end || parsed == 0 || parsed > UINT32_MAX) {
        fprintf(stderr, "Invalid frequency\n");
        return 2;
      }
      frequency = (uint32_t)parsed;
    }
    if (argc >= 5) {
      if (strlen(argv[4]) != 1 || argv[4][0] < '0' || argv[4][0] > '3') {
        fprintf(stderr, "Invalid UART index\n");
        return 2;
      }
      uart = argv[4][0];
    }
  } else {
    for (int i = 1; i < argc; ++i) {
      if (!strcmp(argv[i], "--firmware") && i + 1 < argc) {
        firmware_path = argv[++i];
      } else if (!strcmp(argv[i], "--mcu") && i + 1 < argc) {
        mcu = argv[++i];
      } else if (!strcmp(argv[i], "--frequency") && i + 1 < argc) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[++i], &end, 10);
        if (!end || *end || parsed == 0 || parsed > UINT32_MAX) {
          fprintf(stderr, "Invalid frequency\n");
          return 2;
        }
        frequency = (uint32_t)parsed;
      } else if (!strcmp(argv[i], "--pty-file") && i + 1 < argc) {
        pty_file_path = argv[++i];
      } else if (!strcmp(argv[i], "--pause")) {
        paused = true;
      } else if (!strcmp(argv[i], "--uart") && i + 1 < argc) {
        const char *value = argv[++i];
        if (strlen(value) != 1 || value[0] < '0' || value[0] > '3') {
          fprintf(stderr, "Invalid UART index\n");
          return 2;
        }
        uart = value[0];
      } else {
        usage(argv[0]);
        return 2;
      }
    }
  }
  if (!firmware_path) {
    usage(argv[0]);
    return 2;
  }

  struct avr_t *avr = avr_make_mcu_by_name(mcu);
  if (!avr) {
    fprintf(stderr, "Unknown simavr MCU: %s\n", mcu);
    return 2;
  }
  avr->frequency = frequency;
  if (avr_init(avr) != 0) {
    fprintf(stderr, "Unable to initialize MCU: %s\n", mcu);
    return 1;
  }

  elf_firmware_t firmware;
  memset(&firmware, 0, sizeof(firmware));
  if (elf_read_firmware(firmware_path, &firmware) != 0) {
    fprintf(stderr, "Unable to read firmware ELF: %s\n", firmware_path);
    avr_terminate(avr);
    return 1;
  }
  firmware.frequency = frequency;
  avr_load_firmware(avr, &firmware);

  char pty_path[256] = {0};
  if (openpty(&g_pty_master, &g_pty_slave, pty_path, NULL, NULL) != 0) {
    fprintf(stderr, "openpty failed: %s\n", strerror(errno));
    avr_terminate(avr);
    return 1;
  }
  struct termios settings;
  if (tcgetattr(g_pty_slave, &settings) != 0) {
    fprintf(stderr, "tcgetattr failed: %s\n", strerror(errno));
    close(g_pty_slave);
    close(g_pty_master);
    avr_terminate(avr);
    return 1;
  }
  cfmakeraw(&settings);
  if (tcsetattr(g_pty_slave, TCSANOW, &settings) != 0) {
    fprintf(stderr, "tcsetattr failed: %s\n", strerror(errno));
    close(g_pty_slave);
    close(g_pty_master);
    avr_terminate(avr);
    return 1;
  }

  const int flags = fcntl(g_pty_master, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(g_pty_master, F_SETFL, flags | O_NONBLOCK);

  uint32_t uart_flags = 0;
  avr_ioctl(avr, AVR_IOCTL_UART_GET_FLAGS(uart), &uart_flags);
  uart_flags &= ~AVR_UART_FLAG_STDIO;
  avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS(uart), &uart_flags);

  avr_irq_t *tx = avr_io_getirq(
      avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUTPUT);
  avr_irq_t *rx = avr_io_getirq(
      avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_INPUT);
  avr_irq_t *xon = avr_io_getirq(
      avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XON);
  avr_irq_t *xoff = avr_io_getirq(
      avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XOFF);
  if (!tx || !rx) {
    fprintf(stderr, "UART%c is unavailable on MCU %s\n", uart, mcu);
    close(g_pty_slave);
    close(g_pty_master);
    avr_terminate(avr);
    return 1;
  }
  avr_irq_register_notify(tx, uart_tx, NULL);
  if (xon) avr_irq_register_notify(xon, uart_xon, NULL);
  if (xoff) avr_irq_register_notify(xoff, uart_xoff, NULL);

  signal(SIGINT, stop_handler);
  signal(SIGTERM, stop_handler);
  signal(SIGUSR1, reset_handler);
  signal(SIGUSR2, continue_handler);
  if (pty_file_path) {
    FILE *pty_file = fopen(pty_file_path, "w");
    if (!pty_file) {
      fprintf(stderr, "Unable to write PTY path file %s: %s\n",
              pty_file_path, strerror(errno));
      close(g_pty_slave);
      close(g_pty_master);
      g_pty_master = -1;
      avr_terminate(avr);
      return 1;
    }
    const int path_write_failed =
        fprintf(pty_file, "%s\n", pty_path) < 0 || fflush(pty_file) != 0;
    const int path_close_failed = fclose(pty_file) != 0;
    if (path_write_failed || path_close_failed) {
      fprintf(stderr, "Unable to persist PTY path to %s\n", pty_file_path);
      close(g_pty_slave);
      close(g_pty_master);
      g_pty_master = -1;
      avr_terminate(avr);
      return 1;
    }
  }

  printf("[SIMAVR] UART%c PTY ready on: %s\n", uart, pty_path);
  fflush(stdout);

  uint8_t input[256];
  while (!g_stop && avr->state != cpu_Done && avr->state != cpu_Crashed) {
    if (g_reset) {
      g_reset = 0;
      g_tx_head = 0;
      g_tx_tail = 0;
      g_rx_head = 0;
      g_rx_tail = 0;
      g_uart_xon = true;
      avr_reset(avr);
    }
    if (g_continue) {
      g_continue = 0;
      paused = false;
    }
    if (paused) {
      usleep(10000);
      continue;
    }
    const ssize_t count = read(g_pty_master, input, sizeof(input));
    if (count > 0) {
      for (ssize_t i = 0; i < count; ++i) {
        const size_t next = (g_rx_head + 1U) % sizeof(g_rx_queue);
        if (next != g_rx_tail) {
          g_rx_queue[g_rx_head] = input[i];
          g_rx_head = next;
        }
      }
    } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
               errno != EINTR && errno != EIO) {
      fprintf(stderr, "PTY read failed: %s\n", strerror(errno));
      g_failure = 1;
      break;
    }

    drain_rx_queue(avr, rx);

    for (size_t step = 0; step < BATCH_INSTRUCTION_CYCLES; ++step) {
      if (avr->state == cpu_Done || avr->state == cpu_Crashed || g_stop ||
          g_reset) {
        break;
      }
      (void)avr_run(avr);
    }

    drain_rx_queue(avr, rx);
    flush_tx_queue();
  }

  // Drain UART bytes queued by the final AVR instructions before closing
  // the PTY. The firmware may enter sleep or terminate immediately after its
  // final UART write, while the host-side queue still contains bytes.
  for (unsigned attempt = 0;
       g_tx_tail != g_tx_head && attempt < 2000;
       ++attempt) {
    size_t available = (g_tx_head + sizeof(g_tx_queue) - g_tx_tail) %
                       sizeof(g_tx_queue);
    size_t contiguous = sizeof(g_tx_queue) - g_tx_tail;
    if (contiguous > available) contiguous = available;
    const ssize_t written =
        write(g_pty_master, &g_tx_queue[g_tx_tail], contiguous);
    if (written > 0) {
      g_tx_tail = (g_tx_tail + (size_t)written) % sizeof(g_tx_queue);
    } else if (written < 0 &&
               (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      usleep(1000);
    } else if (written < 0 && errno == EIO) {
      break;
    } else {
      fprintf(stderr, "Final PTY flush failed: %s\n", strerror(errno));
      g_failure = 1;
      break;
    }
  }
  if (g_tx_tail != g_tx_head) {
    fprintf(stderr, "Warning: dropping %zu queued UART bytes on shutdown\n",
            (g_tx_head + sizeof(g_tx_queue) - g_tx_tail) % sizeof(g_tx_queue));
  }

  if (g_pty_slave >= 0) {
    close(g_pty_slave);
    g_pty_slave = -1;
  }
  close(g_pty_master);
  g_pty_master = -1;
  if (avr->state == cpu_Crashed) {
    fprintf(stderr, "AVR simulation crashed\n");
    g_failure = 1;
  }
  avr_terminate(avr);
  return g_failure ? 1 : 0;
}
