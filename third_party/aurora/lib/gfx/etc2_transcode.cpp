// Texture -> ETC2 (see etc2_transcode.hpp for the policy).
#include "etc2_transcode.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "etc2_encode.hpp"
#include "upload_churn.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

namespace aurora::gfx::etc2 {
namespace {
Module Log{"aurora::etc2"};

enum class Mode { Off, CompressedSourcesOnly, Everything };

std::atomic<int> g_enabled{0};   // settings::g.compressed_textures: 0 auto, 1 off, 2 everything

Mode mode() {
  static const Mode platform_default = [] {
#if defined(__ANDROID__)
    return Mode::CompressedSourcesOnly;   // the phone: the biggest win for no new loss (CMPR is already lossy)
#else
    return Mode::Off;                     // the desktop uploads BC1 directly; nothing to gain
#endif
  }();
  static bool logged = false;
  Mode m = platform_default;
  if (const char* v = getenv("SHADOW_ETC2")) {
    if (*v == '0' || *v == 'f' || *v == 'F') m = Mode::Off;
    else if (*v == '1' || *v == 'a' || *v == 'A' || *v == 't' || *v == 'T') m = Mode::Everything;
  } else {
    switch (g_enabled.load(std::memory_order_relaxed)) {
    case 1: m = Mode::Off; break;
    case 2: m = Mode::Everything; break;
    default: break;
    }
  }
  if (!logged) {
    logged = true;
    const char* name = m == Mode::Off ? "off" : m == Mode::Everything ? "on (every RGB source)" : "on (CMPR only)";
    Log.info("textures: ETC2 {}; the GPU reports ETC2 textures: {}", name,
             webgpu::g_etc2TexturesSupported ? "yes" : "no");
  }
  return m;
}

uint64_t g_before = 0;   // texture bytes the game handed us (RGBA8)
uint64_t g_after = 0;    // and what they became
uint64_t g_textures = 0;

// Per-frame uploads: see upload_churn.hpp.
UploadChurn g_churn;

uint64_t now_ns() {
  using namespace std::chrono;
  return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

// True when this texture is one of a stream of per-frame uploads (and was counted).
bool hot(uint32_t width, uint32_t height, uint32_t mips) {
  bool became = false;
  const bool is_hot = g_churn.hot(width, height, mips, now_ns(), &became);
  if (became) Log.info("ETC2: {}x{} is re-uploaded every frame - keeping it uncompressed", width, height);
  return is_hot;
}
}  // namespace

void set_enabled(int mode) { g_enabled.store(mode, std::memory_order_relaxed); }

bool wants(u32 gxFormat, uint32_t width, uint32_t height, uint32_t mips) {
  const Mode m = mode();
  if (m == Mode::Off) return false;
  if (!webgpu::g_etc2TexturesSupported) return false;
  switch (gxFormat) {
  case GX_TF_CMPR:   // block compressed on the console already
    return !hot(width, height, mips);
  case GX_TF_RGBA8:
  case GX_TF_RGB565:
  case GX_TF_RGB5A3:
  case GX_TF_IA8:
    // exact on the console: only when the user asks (SHADOW_ETC2=1 or the setting)
    return m == Mode::Everything && !hot(width, height, mips);
  default:
    return false;   // palettes, intensity, alpha-only: not worth the loss
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

Stats stats() {
  const Mode m = mode();
  return {.textures = g_textures,
          .before = g_before,
          .after = g_after,
          .mode = m == Mode::Off ? 0 : m == Mode::CompressedSourcesOnly ? 1 : 2};
}

void log_stats() {
  if (g_textures == 0) return;
  const double saved = g_before ? 100.0 * (1.0 - double(g_after) / double(g_before)) : 0.0;
  Log.info("ETC2: {} textures, {:.1f} MiB -> {:.1f} MiB ({:.0f}% less texture data)", g_textures,
           double(g_before) / (1024.0 * 1024.0), double(g_after) / (1024.0 * 1024.0), saved);
}

}  // namespace aurora::gfx::etc2
