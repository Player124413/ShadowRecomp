// ETC2 encoder check: encodes 4x4 blocks (a few textures-worth) and writes them out so a decoder can be pointed at
// them. The decoder is deliberately not in this file: tools/android/etc2_check.py uses an independent ETC2 decoder
// (the "texture2ddecoder" wheel), so a mistake in our bit layout cannot hide behind a matching mistake in a decoder
// written by the same hand.
//
//   g++ -std=c++20 -Ithird_party/aurora/lib tools/android/etc2_check.cpp
//       third_party/aurora/lib/gfx/etc2_encode.cpp -o /tmp/etc2_check
//   /tmp/etc2_check out.bin                      # 16-byte header, then blocks
//   python3 tools/android/etc2_check.py out.bin  # prints PSNR per image
//
// The file: magic "E2CK", uint32 count, uint32 width, uint32 height, then per image: RGBA8 source (w*h*4), the
// encoded blocks (8 or 16 bytes each, RGB or RGBA).
#include "gfx/etc2_encode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Image {
  const char* name;
  int w, h;
  std::vector<uint8_t> rgba;
  bool alpha;   // encode as ETC2 RGBA (with the alpha channel)
};

uint8_t clamp8(float v) { return (uint8_t)std::clamp((int)std::lround(v), 0, 255); }

// A smooth gradient with a hard edge: two things an encoder must get right.
void make_gradient(Image& im) {
  im.rgba.resize(im.w * im.h * 4);
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x) {
      uint8_t* p = &im.rgba[(y * im.w + x) * 4];
      p[0] = clamp8(255.f * x / (im.w - 1));
      p[1] = clamp8(255.f * y / (im.h - 1));
      p[2] = (uint8_t)(((x / 8 + y / 8) & 1) ? 40 : 200);
      p[3] = 255;
    }
}

// Flat colours and thin lines: what the game's UI art looks like (the hardest thing for a block compressor).
void make_ui(Image& im) {
  im.rgba.resize(im.w * im.h * 4, 0);
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x) {
      uint8_t* p = &im.rgba[(y * im.w + x) * 4];
      const bool border = x < 2 || y < 2 || x >= im.w - 2 || y >= im.h - 2;
      const bool line = ((x + y) % 17) == 0;
      p[0] = border ? 230 : line ? 255 : 24;
      p[1] = border ? 190 : line ? 255 : 26;
      p[2] = border ? 60 : line ? 255 : 30;
      p[3] = (x < im.w / 2) ? 255 : (uint8_t)(x * 255 / (im.w - 1));   // a smooth alpha ramp on the right half
    }
  im.alpha = true;
}

// Noise: the worst case, the number that should look bad (it shows the encoder is not cheating).
void make_noise(Image& im) {
  im.rgba.resize(im.w * im.h * 4);
  uint32_t s = 12345;
  for (size_t i = 0; i < im.rgba.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    im.rgba[i] = (uint8_t)(s >> 24);
  }
  for (int i = 3; i < (int)im.rgba.size(); i += 4) im.rgba[i] = 255;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "etc2_blocks.bin";
  std::vector<Image> images(3);
  images[0] = Image{"gradient", 64, 64, {}, false};
  images[1] = Image{"ui", 64, 64, {}, false};
  images[2] = Image{"noise", 64, 64, {}, false};
  make_gradient(images[0]);
  make_ui(images[1]);
  make_noise(images[2]);

  FILE* f = fopen(path, "wb");
  if (!f) {
    printf("cannot write %s\n", path);
    return 1;
  }
  const uint32_t n = (uint32_t)images.size();
  fwrite("E2CK", 1, 4, f);
  fwrite(&n, 4, 1, f);

  for (const auto& im : images) {
    const size_t block_size = im.alpha ? 16 : 8;
    const uint32_t w = (uint32_t)im.w, h = (uint32_t)im.h, bs = (uint32_t)block_size;
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    fwrite(&bs, 4, 1, f);
    std::vector<uint8_t> blocks;
    blocks.reserve((im.w / 4) * (im.h / 4) * block_size);
    uint8_t tmp[16];
    for (int by = 0; by < im.h; by += 4)
      for (int bx = 0; bx < im.w; bx += 4) {
        const uint8_t* src = &im.rgba[(by * im.w + bx) * 4];
        const size_t stride = (size_t)im.w * 4;
        if (im.alpha) {
          aurora::gfx::etc2::encode_rgba(src, stride, tmp);
          blocks.insert(blocks.end(), tmp, tmp + 16);
        } else {
          aurora::gfx::etc2::encode_rgb(src, stride, tmp);
          blocks.insert(blocks.end(), tmp, tmp + 8);
        }
      }
    fwrite(im.rgba.data(), 1, im.rgba.size(), f);
    fwrite(blocks.data(), 1, blocks.size(), f);
    printf("%-9s %dx%d %s -> %zu bytes of blocks (source %zu)\n", im.name, im.w, im.h, im.alpha ? "RGBA" : "RGB",
           blocks.size(), im.rgba.size());
  }
  fclose(f);
  printf("wrote %s\n", path);
  return 0;
}
