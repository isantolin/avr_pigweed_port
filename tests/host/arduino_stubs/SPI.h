#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "Arduino.h"

#define MSBFIRST 1
#define LSBFIRST 0
#define SPI_MODE0 0x00
#define SPI_MODE1 0x04
#define SPI_MODE2 0x08
#define SPI_MODE3 0x0C

class SPISettings {
 public:
  constexpr SPISettings(uint32_t clock_hz,
                        uint8_t bit_order,
                        uint8_t data_mode)
      : clock_hz(clock_hz), bit_order(bit_order), data_mode(data_mode) {}

  uint32_t clock_hz;
  uint8_t bit_order;
  uint8_t data_mode;
};

class SPIClass {
 public:
  bool begun = false;
  bool in_transaction = false;
  SPISettings last_settings{4000000U, MSBFIRST, SPI_MODE0};
  std::vector<uint8_t> tx_log;
  std::vector<uint8_t> rx_feed;
  size_t rx_index = 0;

  void begin() { begun = true; }
  void end() { begun = false; }

  void beginTransaction(const SPISettings& settings) {
    in_transaction = true;
    last_settings = settings;
  }

  uint8_t transfer(uint8_t value) {
    tx_log.push_back(value);
    if (rx_index < rx_feed.size()) {
      return rx_feed[rx_index++];
    }
    return value;
  }

  void endTransaction() { in_transaction = false; }
};

inline SPIClass SPI;
