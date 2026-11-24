#pragma once
#include <stdint.h>
#include <vector>
#include "Arduino.h"

struct SPISettings {
  uint32_t clock_hz = 4000000U;
  uint8_t bit_order = MSBFIRST;
  uint8_t data_mode = SPI_MODE0;
  constexpr SPISettings() = default;
  constexpr SPISettings(uint32_t hz, uint8_t order, uint8_t mode)
      : clock_hz(hz), bit_order(order), data_mode(mode) {}
};

class SPIClass {
 public:
  bool begun = false;
  bool in_tx = false;
  SPISettings last_settings{};
  std::vector<uint8_t> tx_log;
  std::vector<uint8_t> rx_feed;
  size_t rx_idx = 0;

  void begin() { begun = true; }
  void end() { begun = false; }
  void beginTransaction(const SPISettings& s) {
    in_tx = true;
    last_settings = s;
  }
  uint8_t transfer(uint8_t b) {
    tx_log.push_back(b);
    if (rx_idx < rx_feed.size()) {
      return rx_feed[rx_idx++];
    }
    return static_cast<uint8_t>(b ^ 0xFFU);
  }
  void endTransaction() { in_tx = false; }
};

inline SPIClass SPI;
