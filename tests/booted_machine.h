#pragma once
// A factory disk image booted to its settled state, computed once per image per
// test run and shared by every test file that needs a booted machine.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "asset_paths.h"
#include "mirage/clocks.h"
#include "mirage/machine.h"

namespace mirage_test {

inline std::vector<uint8_t> loadBytes(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline std::vector<uint8_t> bootRomBytes() {
  return loadBytes(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
}

// Boots a factory disk image (the OS auto-loads the lower sound) until it has
// settled, and returns the machine state at that point. Settled = the drive
// motor is off, the display has been lit for a second and no sector has been
// read for two (docs/hardware/boot-rom.md 13.5). SND-A01 settles at 14.6 s
// (13.6); capped at 90 s of emulated time.
struct Booted {
  std::vector<uint8_t> state;
  double seconds = 0;
};

inline const Booted& bootedSnapshot(const std::string& image) {
  static std::map<std::string, Booted> cache;
  auto it = cache.find(image);
  if (it != cache.end()) return it->second;
  const uint64_t second = mirage::kCpuClockHz;
  const auto rom = bootRomBytes();
  mirage::Machine m(rom);
  m.insertDisk(mirage::DiskImage::loadFile(assetPath("Factory Library/IMG/" + image)));
  m.powerOn();
  uint64_t lastSectors = 0, quiet = 0, lit = 0;
  for (uint64_t t = 0; t < 9000; ++t) {
    m.runCycles(second / 100);
    const mirage::DisplayState d = m.display();
    lit = (d.left || d.right) ? lit + 1 : 0;
    quiet = m.fdc().sectorsRead() == lastSectors ? quiet + 1 : 0;
    lastSectors = m.fdc().sectorsRead();
    if (lit >= 100 && quiet >= 200 && !m.driveActive()) break;
  }
  return cache[image] = {m.saveState(), double(m.cycles()) / double(second)};
}

// A fresh Machine (same ROM) restored from the settled state.
inline std::unique_ptr<mirage::Machine> bootedMachine(const std::string& image) {
  auto m = std::make_unique<mirage::Machine>(bootRomBytes());
  m->loadState(bootedSnapshot(image).state);
  return m;
}

}  // namespace mirage_test
