// Texture -> ETC2 (see etc2_transcode.hpp for the policy).
#include "etc2_transcode.hpp"

#include <algorithm>
#include <cstdlib>

#include "etc2_encode.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

namespace aurora::gfx::etc2 {
namespace {
Module Log{"aurora::etc2"};

enum class Mode { Off, CompressedSourcesOnly, Everything };

Mode mode() {
  static const Mode m = [] {
    const char* v = getenv("SHADOW_ETC2");
    if (v != nullptr && (*v == '0' || *v == 'f' || *v == 'F')) return Mode::Off;
    if (v != nullptr && (*v == '1' || *v == 'a' || *v == 'A' || *v == 't' || *v == 'T')) return Mode::Everything;
#if defined(__ANDROID__)
    return Mode::CompressedSourcesOnly;   // the phone: the biggest win for no new loss (CMPR is already lossy)
#else
    return Mode::Off;                     // the desktop uploads BC1 directly; nothing to gain
#endif
  }();
  return m;
}

uint64_t g_before = 0;   // texture bytes the game handed us (RGBA8)
uint64_t g_after = 0;    // and what they became
uint64_t g_textures = 0;
}  // namespace

bool wants(u32 gxFormat) {
  const Mode m = mode();
  if (m == Mode::Off) return false;
  if (!webgpu::g_etc2TexturesSupported) return false;
  switch (gxFormat) {
  case GX_TF_CMPR:   // block compressed on the console already
    return true;
  case GX_TF_RGBA8:
  case GX_TF_RGB565:
  case GX_TF_RGB5A3:
  case GX_TF_IA8:
    return m == Mode::Everything;   // exact on the console: only when the user asks (SHADOW_ETC2=1)
  default:
    return false;                   // palettes, intensity, alpha-only: not worth the loss
  }
}

Result transcode(uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> rgba) {
  const size_t rgbBytes = image_size(width, height, mips);
  if (rgbBytes == 0 || rgba.size() < size_t(width) * height * 4) return {};
  // Only pay for EAC alpha when the source actually has transparency: it doubles the block size.
  bool hasAlpha = false;
  for (size_t i = 3, n = rgba.size(); i < n; i += 4) {
    if (rgba[i] != 0xFF) {
      hasAlpha = true;
      break;
    }
  }
  ByteBuffer out = hasAlpha ? ByteBuffer{rgbBytes * 2} : ByteBuffer{rgbBytes};
  if (out.size() != (hasAlpha ? rgbBytes * 2 : rgbBytes)) return {};
  if (hasAlpha) {
    encode_image_rgba(rgba.data(), width, height, mips, out.data());
  } else {
    encode_image(rgba.data(), width, height, mips, out.data());
  }
  g_before += rgba.size();
  g_after += out.size();
  if ((++g_textures % 64) == 0) log_stats();
  return {.data = std::move(out), .hasAlpha = hasAlpha};
}

void log_stats() {
  if (g_textures == 0) return;
  const double saved = g_before ? 100.0 * (1.0 - double(g_after) / double(g_before)) : 0.0;
  Log.info("ETC2: {} textures, {:.1f} MiB -> {:.1f} MiB ({:.0f}% less texture data)", g_textures,
           double(g_before) / (1024.0 * 1024.0), double(g_after) / (1024.0 * 1024.0), saved);
}

}  // namespace aurora::gfx::etc2
