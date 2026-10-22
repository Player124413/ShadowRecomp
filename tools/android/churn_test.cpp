// Test for upload_churn.hpp: which texture uploads count as "every frame" and which do not.
//
//   g++ -std=c++20 -Ithird_party/aurora/lib tools/android/churn_test.cpp -o /tmp/churn_test && /tmp/churn_test
//
// No GPU, no aurora headers beyond the header itself, and a fake clock, so the timing rules are checked exactly.
#include "gfx/upload_churn.hpp"

#include <cstdio>
#include <vector>

using aurora::gfx::etc2::UploadChurn;

namespace {
int g_failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
  }
}
}  // namespace

int main() {
  // A movie: one new 640x448 frame every frame time. It has to stop being transcoded quickly, and say so once.
  {
    UploadChurn churn;
    uint64_t now = 1000000000ull;
    std::vector<bool> hot;
    int announcements = 0;
    for (int frame = 0; frame < 30; ++frame) {
      now += 33333333ull;   // 30 fps
      bool became = false;
      hot.push_back(churn.hot(640, 448, 1, now, &became));
      if (became) ++announcements;
    }
    for (size_t i = 0; i < 4; ++i) expect(!hot[i], "the first textures of a new size are still transcoded");
    bool became_by_frame_10 = false;
    for (size_t i = 0; i < 10; ++i) became_by_frame_10 = became_by_frame_10 || hot[i];
    expect(became_by_frame_10, "a per-frame upload becomes hot within ten frames");
    for (size_t i = 10; i < hot.size(); ++i) expect(hot[i], "and stays hot while the stream runs");
    expect(announcements == 1, "the change is announced once, not every frame");
  }

  // The stream ends: after a quiet period the size is transcoded again.
  {
    UploadChurn churn;
    uint64_t now = 1000000000ull;
    for (int frame = 0; frame < 10; ++frame) churn.hot(640, 448, 1, now += 33333333ull);
    expect(churn.hot(640, 448, 1, now), "still hot while the stream runs");
    now += UploadChurn::kQuietFor + 1000000ull;
    expect(!churn.hot(640, 448, 1, now), "a size that stopped streaming is transcoded again");
  }

  // A level load: thirty textures of the same size in one burst is not a stream, and none of them may lose ETC2.
  {
    UploadChurn churn;
    uint64_t now = 5000000000ull;
    for (int i = 0; i < 30; ++i) {
      expect(!churn.hot(256, 256, 10, now += 500000ull), "a burst of textures of one size stays compressed");
    }
    for (int i = 0; i < 30; ++i) {
      expect(!churn.hot(512, 512, 10, now += 5000000ull), "a longer burst stays compressed too");
    }
  }

  // Many small sprites in a row are not a video stream.
  {
    UploadChurn churn;
    uint64_t now = 0;
    for (int i = 0; i < 60; ++i) expect(!churn.hot(32, 32, 1, now += 1000000ull), "small textures are never hot");
  }

  // One-off textures of different sizes, and the same size with a different mip count, are separate counters.
  {
    UploadChurn churn;
    uint64_t now = 0;
    expect(!churn.hot(512, 512, 10, now += 1000000ull), "a one-off texture is transcoded");
    expect(!churn.hot(256, 256, 9, now += 1000000ull), "a different size has its own counter");
    expect(!churn.hot(640, 448, 9, now += 1000000ull), "the mip count is part of the key");
  }

  // Several streamed textures at once (a movie plus its overlay) all get caught.
  {
    UploadChurn churn;
    uint64_t now = 0;
    for (int frame = 0; frame < 20; ++frame) {
      now += 33333333ull;
      churn.hot(320, 240, 1, now);
      churn.hot(160, 120, 1, now);
      churn.hot(80, 60, 1, now);
    }
    expect(churn.hot(320, 240, 1, now) && churn.hot(160, 120, 1, now) && churn.hot(80, 60, 1, now),
           "every size of a multi-texture stream is caught");
  }

  // The slot table is finite: twenty streams in a row, each of which has to be recognised while it runs.
  {
    UploadChurn churn;
    uint64_t now = 0;
    for (int size = 0; size < 20; ++size) {
      const uint32_t w = 64 + uint32_t(size) * 4;
      bool hot = false;
      for (int frame = 0; frame < 20; ++frame) hot = churn.hot(w, 64, 1, now += 33333333ull);
      expect(hot, "each stream is recognised while it runs");
    }
  }

  if (g_failures == 0) std::printf("upload churn: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
