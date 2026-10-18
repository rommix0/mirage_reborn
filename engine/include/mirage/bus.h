#pragma once
#include <cstdint>

namespace mirage {

// The CPU's view of the address space. Reads may have side effects (I/O).
struct Bus {
  virtual uint8_t read(uint16_t addr) = 0;
  virtual void write(uint16_t addr, uint8_t value) = 0;
  virtual ~Bus() = default;
};

}  // namespace mirage
