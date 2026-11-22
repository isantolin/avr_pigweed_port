#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <stdint.h>

static void uart0_init(void) {
  const uint16_t ubrr = (uint16_t)((F_CPU / (16UL * 115200UL)) - 1UL);
  UBRR0H = (uint8_t)(ubrr >> 8);
  UBRR0L = (uint8_t)(ubrr & 0xffU);
  UCSR0B = (uint8_t)((1U << RXEN0) | (1U << TXEN0));
  UCSR0A = (uint8_t)(1U << TXC0);
  UCSR0C = (uint8_t)((1U << UCSZ01) | (1U << UCSZ00));
}

static void uart1_init(void) {
  const uint16_t ubrr = (uint16_t)((F_CPU / (16UL * 115200UL)) - 1UL);
  UBRR1H = (uint8_t)(ubrr >> 8);
  UBRR1L = (uint8_t)(ubrr & 0xffU);
  UCSR1B = (uint8_t)((1U << RXEN1) | (1U << TXEN1));
  UCSR1A = (uint8_t)(1U << TXC1);
  UCSR1C = (uint8_t)((1U << UCSZ11) | (1U << UCSZ10));
}

static void uart0_write(const char *message) {
  for (uint8_t i = 0; message[i] != '\0'; ++i) {
    while ((UCSR0A & (1U << UDRE0)) == 0U) {
    }
    UDR0 = (uint8_t)message[i];
  }
  while ((UCSR0A & (1U << TXC0)) == 0U) {
  }
}

static void uart1_write(const char *message) {
  for (uint8_t i = 0; message[i] != '\0'; ++i) {
    while ((UCSR1A & (1U << UDRE1)) == 0U) {
    }
    UDR1 = (uint8_t)message[i];
  }
  while ((UCSR1A & (1U << TXC1)) == 0U) {
  }
}

int main(void) {
  uart0_init();
  uart1_init();
  sei();

  uart0_write("PW_EMU_UART0_READY\r\n");
  uart1_write("PW_EMU_UART1_READY\r\n");

  uint8_t done0 = 0U;
  uint8_t done1 = 0U;
  while (!done0 || !done1) {
    if (!done0 && (UCSR0A & (1U << RXC0)) != 0U) {
      const uint8_t b = UDR0;
      if (b == '0') {
        uart0_write("PW_EMU_UART0_PASS\r\n");
        done0 = 1U;
      } else {
        uart0_write("PW_EMU_UART0_FAIL\r\n");
        done0 = 1U;
      }
    }
    if (!done1 && (UCSR1A & (1U << RXC1)) != 0U) {
      const uint8_t b = UDR1;
      if (b == '1') {
        uart1_write("PW_EMU_UART1_PASS\r\n");
        done1 = 1U;
      } else {
        uart1_write("PW_EMU_UART1_FAIL\r\n");
        done1 = 1U;
      }
    }
  }

  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  for (;;) {
    sleep_cpu();
  }
}
