#pragma once
// A factory disk image booted to its settled state, computed once per image per
// test run and shared by every test file that needs a booted machine.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

// Runs a powered-on machine until it has settled (the rule above).
inline double settleBoot(mirage::Machine& m) {
  const uint64_t second = mirage::kCpuClockHz;
  const uint64_t start = m.cycles();
  uint64_t lastSectors = m.fdc().sectorsRead(), quiet = 0, lit = 0;
  for (uint64_t t = 0; t < 9000; ++t) {
    m.runCycles(second / 100);
    const mirage::DisplayState d = m.display();
    lit = (d.left || d.right) ? lit + 1 : 0;
    quiet = m.fdc().sectorsRead() == lastSectors ? quiet + 1 : 0;
    lastSectors = m.fdc().sectorsRead();
    if (lit >= 100 && quiet >= 200 && !m.driveActive()) break;
  }
  return double(m.cycles() - start) / double(second);
}

// The disk image at `assetRel` (relative to MIRAGE_ASSET_ROOT), booted and
// settled; computed once per path per test run.
inline const Booted& bootedSnapshotAt(const std::string& assetRel) {
  static std::map<std::string, Booted> cache;
  auto it = cache.find(assetRel);
  if (it != cache.end()) return it->second;
  const auto rom = bootRomBytes();
  mirage::Machine m(rom);
  m.insertDisk(mirage::DiskImage::loadFile(assetPath(assetRel)));
  m.powerOn();
  const double seconds = settleBoot(m);
  return cache[assetRel] = {m.saveState(), seconds};
}

// A fresh Machine (same ROM) restored from the settled state.
inline std::unique_ptr<mirage::Machine> bootedMachineAt(const std::string& assetRel) {
  auto m = std::make_unique<mirage::Machine>(bootRomBytes());
  m->loadState(bootedSnapshotAt(assetRel).state);
  return m;
}

// A factory image under Factory Library/IMG.
inline const Booted& bootedSnapshot(const std::string& image) {
  return bootedSnapshotAt("Factory Library/IMG/" + image);
}

inline std::unique_ptr<mirage::Machine> bootedMachine(const std::string& image) {
  return bootedMachineAt("Factory Library/IMG/" + image);
}

// ---- driving the OS from the panel -----------------------------------------

inline void runSeconds(mirage::Machine& m, double s) {
  m.runCycles(uint64_t(s * double(mirage::kCpuClockHz) + 0.5));
}

// Presses each key in turn: down for holdS, up, then gapS before the next.
inline void pressKeys(mirage::Machine& m, std::initializer_list<mirage::PanelKey> keys,
                      double holdS = 0.15, double gapS = 0.25) {
  for (const mirage::PanelKey k : keys) {
    m.setKey(k, true);
    runSeconds(m, holdS);
    m.setKey(k, false);
    runSeconds(m, gapS);
  }
}

inline mirage::PanelKey digitKey(int d) {
  using mirage::PanelKey;
  static constexpr PanelKey keys[10] = {PanelKey::K0, PanelKey::K1, PanelKey::K2, PanelKey::K3,
                                        PanelKey::K4, PanelKey::K5, PanelKey::K6, PanelKey::K7,
                                        PanelKey::K8, PanelKey::K9};
  return keys[d];
}

// OS 3.2 (musician's manual, "Selecting and changing a parameter"): Param, the
// two digits, Value. Returns the display, which then shows the value (right
// decimal point lit).
inline mirage::DisplayState showParameter(mirage::Machine& m, int number) {
  pressKeys(m, {mirage::PanelKey::Param, digitKey(number / 10 % 10), digitKey(number % 10),
                mirage::PanelKey::Value});
  return m.display();
}

// Runs until the drive has been idle for 2 s, at most 60 s; REQUIREs it got there.
inline void runUntilDriveIdle(mirage::Machine& m) {
  const uint64_t tick = mirage::kCpuClockHz / 100;
  int idle = 0;
  for (int t = 0; t < 6000 && idle < 200; ++t) {
    m.runCycles(tick);
    idle = m.driveActive() ? 0 : idle + 1;
  }
  REQUIRE(idle >= 200);
}

// Save Lower (parameter 11) to disk location 1 (musician's manual, "Saving a
// sound on a formatted diskette"): Param, 1, 1 (display "SL"), 1 ("S1"), Enter.
inline void osSaveLowerSound(mirage::Machine& m) {
  using mirage::PanelKey;
  pressKeys(m, {PanelKey::Param, PanelKey::K1, PanelKey::K1, PanelKey::K1, PanelKey::Enter});
  runUntilDriveIdle(m);
}

// Load Lower, the sound number (1-3) and Enter ("Changing sounds").
inline void osLoadLowerSound(mirage::Machine& m, int sound) {
  using mirage::PanelKey;
  pressKeys(m, {PanelKey::LoadLower, digitKey(sound), PanelKey::Enter});
  runUntilDriveIdle(m);
}

}  // namespace mirage_test
