#pragma once
// Which texture uploads are worth transcoding to ETC2 - and which are not, because the game re-uploads them every frame.
//
// A movie (a .sfd stream) hands over a new frame texture every frame, and a few other textures the game rewrites per
// frame do the same. Transcoding those would run the block encoder on the render thread once per frame forever, which
// costs far more than the RGBA8 upload it saves: the picture would stutter for a "saving" nobody can see. Every size
// is counted here instead, and one that produces more than kHits new textures inside a second - spread over at least
// kMinSpan, so a level load that makes thirty same-sized textures in one burst is not mistaken for a video stream - is
// left uncompressed for as long as it keeps doing that. kQuietFor of silence clears it again, so a texture that turns
// out to be one-off after all goes back to being compressed.
//
// The state machine is header-only and takes its clock as an argument, so tools/android/churn_test.cpp can drive it
// through a fake clock and check the timing rules exactly, without a GPU.
#include <cstddef>
#include <cstdint>

namespace aurora::gfx::etc2 {

class UploadChurn {
 public:
  static constexpr size_t kSlots = 12;
  static constexpr uint32_t kHits = 5;
  // Small textures are not tracked: a sprite sheet legitimately makes many same-sized textures in a row, and 32x32 of
  // RGBA8 costs nothing to keep. Anything big enough to be a video frame is.
  static constexpr uint32_t kMinPixels = 64 * 64;
  static constexpr uint64_t kSecond = 1000000000ull;
  static constexpr uint64_t kQuietFor = 2 * kSecond;
  // The repeats also have to be spread out over at least this long (see the comment above).
  static constexpr uint64_t kMinSpan = 200 * 1000 * 1000ull;

  // Counts one texture of this size and says whether it should stay uncompressed. `became_hot`, when given, is set on
  // the single call where the size crosses the line.
  bool hot(uint32_t width, uint32_t height, uint32_t mips, uint64_t now_ns, bool* became_hot = nullptr) {
    if (became_hot != nullptr) *became_hot = false;
    if (uint64_t(width) * height < kMinPixels) return false;

    Slot* slot = nullptr;
    Slot* spare = nullptr;
    for (Slot& s : slots_) {
      if (s.used && s.width == width && s.height == height && s.mips == mips) {
        slot = &s;
        break;
      }
      if (spare == nullptr && (!s.used || now_ns - s.last_seen > kQuietFor)) spare = &s;
    }
    if (slot == nullptr) {
      // A new size: reuse a free or expired slot, else the least recently used one.
      slot = spare;
      if (slot == nullptr) {
        slot = &slots_[0];
        for (Slot& s : slots_) {
          if (s.used && s.last_seen < slot->last_seen) slot = &s;
        }
      }
      *slot = Slot{.used = true,
                   .hot = false,
                   .width = width,
                   .height = height,
                   .mips = mips,
                   .hits = 0,
                   .window_start = now_ns,
                   .last_seen = now_ns};
    } else if (now_ns - slot->last_seen > kQuietFor) {
      // It stopped for a while: back to normal, and this is the first hit of a new count.
      slot->hot = false;
      slot->hits = 0;
      slot->window_start = now_ns;
    }

    if (now_ns - slot->window_start > kSecond) {
      slot->window_start = now_ns;
      slot->hits = 0;
    }
    slot->last_seen = now_ns;
    if (!slot->hot && ++slot->hits >= kHits && now_ns - slot->window_start >= kMinSpan) {
      slot->hot = true;
      if (became_hot != nullptr) *became_hot = true;
    }
    return slot->hot;
  }

 private:
  struct Slot {
    bool used = false;
    bool hot = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 0;
    uint32_t hits = 0;
    uint64_t window_start = 0;
    uint64_t last_seen = 0;
  };
  Slot slots_[kSlots] = {};
};

}  // namespace aurora::gfx::etc2
