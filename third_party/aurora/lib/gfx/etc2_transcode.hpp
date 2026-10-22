#pragma once
// Texture -> ETC2 (Android): see etc2_encode.hpp for the encoder and why it exists.
//
// Policy, kept deliberately narrow:
//   * only formats the console already stored block-compressed, i.e. CMPR (= BC1): those were 4 bits per pixel on the
//     console, so re-encoding them to ETC2 keeps the same class of fidelity instead of inventing new loss;
//   * SHADOW_ETC2=1 forces every RGB source through it (RGB565/RGB5A3/RGBA8 too), SHADOW_ETC2=0 turns it off, and
//     "all" is the same as 1 - the tests use the variable;
//   * on Android it is on by default, elsewhere off: the desktop can upload BC1 to the GPU directly, a phone cannot;
//   * nothing happens unless the adapter reports the ETC2 feature, so a GPU without it keeps the old RGBA8 path;
//   * a size that keeps producing new textures - a movie stream re-uploading its frame, a texture the game rewrites
//     every frame - is left uncompressed: the encode would run once per frame, and paying that on the render thread
//     costs more than the bandwidth it saves (see upload_churn.hpp).
//
// The win is four to eight times less texture memory and texture bandwidth, measured and logged as a running total.
#include <cstddef>
#include <cstdint>

#include "texture.hpp"

namespace aurora::gfx::etc2 {

// True when this texture should be transcoded (see the policy above). Counts the texture: a size that appears again
// and again within a second is a per-frame upload and stops being transcoded.
bool wants(u32 gxFormat, uint32_t width, uint32_t height, uint32_t mips);

struct Result {
  ByteBuffer data;             // empty on failure (the caller then keeps RGBA8)
  bool hasAlpha = false;       // true = EAC alpha blocks present, so the GPU format must be ETC2RGBA8
};

// RGBA8 (all mips, tightly packed, mip 0 first) -> ETC2 blocks in the same order. A texture that actually uses its
// alpha channel keeps it (EAC), everything else goes to plain ETC2 RGB at half the size again.
Result transcode(uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> rgba);

// Running totals, logged every so often: how much texture data has been saved so far.
void log_stats();

// The same totals for the diagnostics overlay (settings_ui.cpp): how many textures went through the encoder, what they
// were worth before and after, and which policy is in effect (0 off, 1 CMPR only, 2 everything).
struct Stats {
  uint64_t textures;
  uint64_t before;
  uint64_t after;
  int mode;
};
Stats stats();

// The player's choice from the settings menu: 0 = auto (the policy above), 1 = never transcode, 2 = transcode
// everything ETC2 can express. SHADOW_ETC2 in the environment still wins over both.
//
// It exists because a driver that mis-samples ETC2 would turn every world texture black while the HUD - paletted or
// RGBA art, not transcoded - kept drawing, which is a picture no player can diagnose. One tap in Display > Picture
// drops back to 32-bit textures without losing anything else.
void set_enabled(int mode);

}  // namespace aurora::gfx::etc2
