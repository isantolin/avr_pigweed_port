#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <stdint.h>

static void uart_init(void) {
  const uint16_t ubrr = (uint16_t)((F_CPU / (16UL * 115200UL)) - 1UL);
  UBRR0H = (uint8_t)(ubrr >> 8);
  UBRR0L = (uint8_t)(ubrr & 0xffU);
  UCSR0B = (uint8_t)((1U << RXEN0) | (1U << TXEN0));
  UCSR0A = (uint8_t)(1U << TXC0);
  UCSR0C = (uint8_t)((1U << UCSZ01) | (1U << UCSZ00));
}

static void uart_write(const char *message) {
  for (uint8_t i = 0; message[i] != '\0'; ++i) {
    while ((UCSR0A & (1U << UDRE0)) == 0U) {
    }
    UDR0 = (uint8_t)message[i];
  }
  while ((UCSR0A & (1U << TXC0)) == 0U) {
  }
}

int main(void) {
  uart_init();

  // The host opens the PTY before resuming the MCU, so READY confirms that
  // UART RX is configured before the host sends its start byte.
  uart_write("PW_EMU_UART_READY\\r\\n");
  while ((UCSR0A & (1U << RXC0)) == 0U) {
  }
  const uint8_t start = UDR0;
  if (start == 'G') {
    uart_write("PW_EMU_UART_PASS\r\n");
  } else {
    uart_write("PW_EMU_UART_FAIL\r\n");
  }

  cli();
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  for (;;) {
    sleep_cpu();
  }
}
