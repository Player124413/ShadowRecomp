#pragma once
// Texture -> ETC2 (Android): see etc2_encode.hpp for the encoder and why it exists.
//
// Policy, kept deliberately narrow:
//   * only formats the console already stored block-compressed, i.e. CMPR (= BC1): those were 4 bits per pixel on the
//     console, so re-encoding them to ETC2 keeps the same class of fidelity instead of inventing new loss;
//   * SHADOW_ETC2=1 forces every RGB source through it (RGB565/RGB5A3/RGBA8 too), SHADOW_ETC2=0 turns it off, and
//     "all" is the same as 1 - the tests use the variable;
//   * on Android it is on by default, elsewhere off: the desktop can upload BC1 to the GPU directly, a phone cannot;
//   * nothing happens unless the adapter reports the ETC2 feature, so a GPU without it keeps the old RGBA8 path.
//
// The win is four to eight times less texture memory and texture bandwidth, measured and logged as a running total.
#include <cstddef>
#include <cstdint>

#include "texture.hpp"

namespace aurora::gfx::etc2 {

// True when a texture in this GX format should be transcoded (see the policy above).
bool wants(u32 gxFormat);

struct Result {
  ByteBuffer data;             // empty on failure (the caller then keeps RGBA8)
  bool hasAlpha = false;       // true = EAC alpha blocks present, so the GPU format must be ETC2RGBA8
};

// RGBA8 (all mips, tightly packed, mip 0 first) -> ETC2 blocks in the same order. A texture that actually uses its
// alpha channel keeps it (EAC), everything else goes to plain ETC2 RGB at half the size again.
Result transcode(uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> rgba);

// Running totals, logged every so often: how much texture data has been saved so far.
void log_stats();

}  // namespace aurora::gfx::etc2
