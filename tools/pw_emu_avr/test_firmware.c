#include <avr/eeprom.h>
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

static void uart_write_byte(uint8_t value) {
  while ((UCSR0A & (1U << UDRE0)) == 0U) {
  }
  UDR0 = value;
}

static void uart_write(const char *message) {
  for (uint8_t i = 0; message[i] != '\0'; ++i) {
    uart_write_byte((uint8_t)message[i]);
  }
  while ((UCSR0A & (1U << TXC0)) == 0U) {
  }
}

static void uart_write_hex8(uint8_t value) {
  static const char digits[] = "0123456789ABCDEF";
  uart_write_byte((uint8_t)digits[(value >> 4) & 0x0FU]);
  uart_write_byte((uint8_t)digits[value & 0x0FU]);
}

int main(void) {
  DDRB = 0x00U;
  PORTB = 0x00U;
  uart_init();
  // Keep simavr's idle-loop detector from treating the UART RX wait as an
  // intentional firmware halt while the host sends commands.
  sei();

  // The host opens the channel before resuming the MCU, so READY confirms that
  // UART RX is configured before the host sends its command byte.
  uart_write("PW_EMU_UART_READY\r\n");

  for (;;) {
    while ((UCSR0A & (1U << RXC0)) == 0U) {
    }
    const uint8_t cmd = UDR0;
    if (cmd == 'G') {
      uart_write("PW_EMU_UART_PASS\r\n");
      break;
    } else if (cmd == 'P') {
      const uint8_t pinb = PINB;
      uart_write("PINB=");
      uart_write_hex8(pinb);
      uart_write("\r\n");
    } else if (cmd == 'E') {
      const uint8_t ee0 = eeprom_read_byte((const uint8_t *)0);
      uart_write("EE0=");
      uart_write_hex8(ee0);
      uart_write("\r\n");
    } else if (cmd == 'X') {
      // Deliberately jump past ATmega328P flash end (byte 0x8000 > 0x7FFF)
      // so simavr enters cpu_Crashed (avr_sadly_crashed).
      ((void (*)(void))0x4000U)();
    } else {
      uart_write("PW_EMU_UART_FAIL\r\n");
      break;
    }
  }

  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  for (;;) {
    sleep_cpu();
  }
}
