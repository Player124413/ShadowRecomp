// Android: "is there a disc?" and "nothing to import here".
//
// On a phone the importing is the launcher's job (android/app/src/main/java/.../DiscImport.java): the player picks
// their own Shadow the Hedgehog (USA) image with the system file picker, it is copied into the app's folder, and the
// game is started with --disc <image>. That keeps the 1.4 GB of game files out of the native code and out of the
// first frame. Nothing is unpacked: the runtime reads the image in place (disc.cpp).
#include "first_run.h"
#include "sys.h"
#include "plat.h"
#include <cstdint>
#include <cstring>
#include <string>

namespace first_run {
namespace {
constexpr const char* kGameId = "GUPE8P";

bool read_at(const std::string& path, uint64_t off, void* dst, size_t n) {
  FILE* f = plat::open(path, "rb");
  if (!f) return false;
  const bool ok = _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
  fclose(f);
  return ok;
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
}  // namespace

bool disc_present(const std::string& root) {
  if (root.empty()) return false;
  // an extracted disc folder
  if (plat::is_dir(root)) return plat::exists(plat::join(root, "sys" RT_S "main.dol"));
  // a disc image: the boot header names the game, so a wrong file is caught here instead of in the middle of a load
  uint8_t boot[0x440] = {};
  if (!read_at(root, 0, boot, sizeof boot)) return false;
  if (be32(boot + 0x1C) != 0xC2339F3D) {
    logf("[disc] %s is not a GameCube disc image\n", root.c_str());
    return false;
  }
  if (memcmp(boot, kGameId, 6) != 0) {
    logf("[disc] %s is a GameCube disc, but not the USA version of Shadow the Hedgehog (%.6s)\n", root.c_str(), (const char*)boot);
    plat::android_toast("That is a GameCube disc, but not Shadow the Hedgehog (USA).");
    return false;
  }
  if (boot[7] != 0) {
    logf("[disc] %s is USA revision %d; only the first release is supported\n", root.c_str(), (int)boot[7]);
    plat::android_toast("That copy is a later revision: only the first USA release works.");
    return false;
  }
  return true;
}

std::string run(bool) {
  // The launcher imports the disc before the game starts (it shows the copy's progress in its own UI).
  logf("[disc] no game files: the launcher imports the player's own disc image (android/app)\n");
  return "";
}

}  // namespace first_run
