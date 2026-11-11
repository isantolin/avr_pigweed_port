#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <stdint.h>

int main(void) {
  const uint16_t ubrr = (uint16_t)((F_CPU / (16UL * 115200UL)) - 1UL);
  UBRR0H = (uint8_t)(ubrr >> 8);
  UBRR0L = (uint8_t)(ubrr & 0xffU);
  UCSR0B = (uint8_t)(1U << TXEN0);
  UCSR0C = (uint8_t)((1U << UCSZ01) | (1U << UCSZ00));

  static const char message[] = "PW_EMU_UART_PASS\r\n";
  for (uint8_t i = 0; message[i] != '\0'; ++i) {
    while ((UCSR0A & (1U << UDRE0)) == 0U) {
    }
    UDR0 = (uint8_t)message[i];
  }
  while ((UCSR0A & (1U << TXC0)) == 0U) {
  }

  cli();
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  for (;;) {
    sleep_cpu();
  }
}
