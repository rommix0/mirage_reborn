#include "mirage/boot_rom.h"

namespace mirage {

bool isKnownBootRom(const std::array<uint8_t, 20>& sha1) { return sha1 == kKnownBootRomSha1; }

}  // namespace mirage
