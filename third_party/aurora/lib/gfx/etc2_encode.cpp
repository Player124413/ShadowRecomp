// ETC2 block encoder - see etc2_encode.hpp for why and for the block layout.
//
// The search is deliberately simple: for each of the two sub-block splits (flip 0: left/right, flip 1: top/bottom) and
// each of the two base-colour modes (individual 4-bit, differential 5-bit + 3-bit delta) the two base colours are
// chosen from the sub-blocks' minima, averages and maxima, and for each of the eight modifier tables the per-pixel
// selectors that minimise the squared error are picked. The winner is written out.
#include "etc2_encode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace aurora::gfx::etc2 {
namespace {

// The eight ETC modifier tables, in 8-bit units (the format's own constants).
constexpr int kModifiers[8][4] = {
    {2, 8, -2, -8},       {5, 17, -5, -17},     {9, 29, -9, -29},     {13, 42, -13, -42},
    {18, 60, -18, -60},   {24, 80, -24, -80},   {33, 106, -33, -106}, {47, 183, -47, -183},
};

// The sixteen EAC alpha tables (also the format's constants).
constexpr int kAlphaMods[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},  {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12},  {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},  {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},   {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},   {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

// A 5-bit colour channel, expanded the way the hardware does it.
inline int expand5(int v) { return (v << 3) | (v >> 2); }
inline int expand4(int v) { return (v << 4) | v; }

struct Pixel {
  int r, g, b, a;
};

inline Pixel load(const uint8_t* p) { return Pixel{p[0], p[1], p[2], p[3]}; }

// One sub-block's answer: which table, the selectors, and how wrong it is.
struct Sub {
  int table = 0;
  int selector[16] = {};   // 0..3
  int error = 1 << 30;
  int base[3] = {};        // 8-bit, quantised to what the mode can store
};

// Fits one modifier table to a sub-block: alternate "pick the modifier closest to each pixel" with "the base is the
// mean of the pixels minus their modifiers" a few times - the closed-form way to solve this. `quant_bits` is 4 for the
// individual mode and 5 for the differential one (which has more range to spend).
Sub fit_table(const Pixel* px, const int* which, int count, int table, int quant_bits) {
  double base[3] = {0, 0, 0};
  for (int k = 0; k < count; ++k) {
    const Pixel& p = px[which[k]];
    base[0] += p.r;
    base[1] += p.g;
    base[2] += p.b;
  }
  for (double& v : base) v /= count;

  int sel[16] = {};
  for (int iter = 0; iter < 3; ++iter) {
    for (int k = 0; k < count; ++k) {
      const Pixel& p = px[which[k]];
      int bk = 0, bd = 1 << 30;
      for (int m = 0; m < 4; ++m) {
        const int dr = (int)base[0] + kModifiers[table][m] - p.r;
        const int dg = (int)base[1] + kModifiers[table][m] - p.g;
        const int db = (int)base[2] + kModifiers[table][m] - p.b;
        const int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; bk = m; }
      }
      sel[k] = bk;
    }
    double acc[3] = {0, 0, 0};
    for (int k = 0; k < count; ++k) {
      const Pixel& p = px[which[k]];
      const int c[3] = {p.r, p.g, p.b};
      for (int ch = 0; ch < 3; ++ch) acc[ch] += c[ch] - kModifiers[table][sel[k]];
    }
    for (int ch = 0; ch < 3; ++ch) base[ch] = acc[ch] / count;
  }

  Sub out;
  out.table = table;
  const int levels = quant_bits == 5 ? 31 : 15;
  for (int ch = 0; ch < 3; ++ch) {
    const int q = std::clamp((int)std::lround(base[ch] * levels / 255.0), 0, levels);
    out.base[ch] = quant_bits == 5 ? expand5(q) : expand4(q);
  }
  int err = 0;
  for (int k = 0; k < count; ++k) {
    const Pixel& p = px[which[k]];
    const int dr = out.base[0] + kModifiers[table][sel[k]] - p.r;
    const int dg = out.base[1] + kModifiers[table][sel[k]] - p.g;
    const int db = out.base[2] + kModifiers[table][sel[k]] - p.b;
    err += dr * dr + dg * dg + db * db;
    out.selector[which[k]] = sel[k];
  }
  out.error = err;
  return out;
}

// The best table for a sub-block, with the base it wants.
Sub fit_best(const Pixel* px, const int* which, int count, int quant_bits) {
  Sub best;
  for (int t = 0; t < 8; ++t) {
    const Sub r = fit_table(px, which, count, t, quant_bits);
    if (r.error < best.error) best = r;
  }
  return best;
}

struct Trial {
  int error = 1 << 30;
  int flip = 0;
  int diff = 0;               // 1: differential mode
  int base[2][3] = {};        // 4- or 5-bit bases, as the mode stores them
  int table[2] = {};
  int selector[16] = {};
};

// Both base-colour modes for one split.
void try_split(const Pixel* px, int flip, Trial& best) {
  int sub[2][8];
  int n[2] = {0, 0};
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const int i = y * 4 + x;
      const int s = flip ? (y >= 2) : (x >= 2);   // flip 0: left/right halves, flip 1: top/bottom
      sub[s][n[s]++] = i;
    }
  }

  // individual mode: a 4-bit base per sub-block, the halves independent
  {
    Trial t;
    t.flip = flip;
    t.diff = 0;
    int total = 0;
    for (int s = 0; s < 2; ++s) {
      const Sub r = fit_best(px, sub[s], n[s], 4);
      for (int ch = 0; ch < 3; ++ch) t.base[s][ch] = std::clamp((int)std::lround(r.base[ch] * 15 / 255.0), 0, 15);
      t.table[s] = r.table;
      for (int k = 0; k < n[s]; ++k) t.selector[sub[s][k]] = r.selector[sub[s][k]];
      total += r.error;
    }
    t.error = total;
    if (t.error < best.error) best = t;
  }

  // differential mode: one 5-bit base (the second sub-block's) plus a signed 3-bit delta for the first
  {
    Trial t;
    t.flip = flip;
    t.diff = 1;
    int total = 0;
    for (int s = 0; s < 2; ++s) {
      const Sub r = fit_best(px, sub[s], n[s], 5);
      for (int ch = 0; ch < 3; ++ch) t.base[s][ch] = std::clamp((int)std::lround(r.base[ch] * 31 / 255.0), 0, 31);
      t.table[s] = r.table;
      for (int k = 0; k < n[s]; ++k) t.selector[sub[s][k]] = r.selector[sub[s][k]];
      total += r.error;
    }
    bool ok = true;
    for (int ch = 0; ch < 3 && ok; ++ch) {
      // The hardware stores sub-block 0's base and derives sub-block 1 from it, so the delta is base1 - base0 and has
      // to fit in three signed bits.
      const int delta = t.base[1][ch] - t.base[0][ch];
      if (delta < -4 || delta > 3) ok = false;
    }
    if (ok) {
      t.error = total;
      if (t.error < best.error) best = t;
    }
  }
}

void write_block(const Trial& t, uint8_t out[8]) {
  uint32_t lo = 0;
  if (t.diff) {
    for (int ch = 0; ch < 3; ++ch) {
      const int base = t.base[0][ch] & 0x1F;                     // sub-block 0's colour is the one stored
      const int delta = (t.base[1][ch] - t.base[0][ch]) & 0x7;    // sub-block 1 is three signed bits away
      lo |= (uint32_t)base << (ch * 8 + 3);
      lo |= (uint32_t)delta << (ch * 8);
    }
  } else {
    for (int ch = 0; ch < 3; ++ch) {
      lo |= (uint32_t)(t.base[0][ch] & 0xF) << (ch * 8 + 4);     // sub-block 0 in the high nibble
      lo |= (uint32_t)(t.base[1][ch] & 0xF) << (ch * 8);         // sub-block 1 in the low nibble
    }
  }
  lo |= (uint32_t)(t.flip ? 1 : 0) << 24;
  lo |= (uint32_t)(t.diff ? 2 : 0) << 24;
  // ETC1 table order: the *first* sub-block's table sits in the high three bits of the word (byte 3 bits 5-7).
  lo |= (uint32_t)(t.table[0] & 7) << 29;
  lo |= (uint32_t)(t.table[1] & 7) << 26;

  // Pixel indices are stored column by column, not row by row: pixel (x, y) owns bit (x * 4 + y). The second bit of
  // the two-bit selector lives 16 bits further up (bytes 6-7), and the selector's *high* bit is the lower one.
  uint32_t hi = 0;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const int i = x * 4 + y;
      const int sel = t.selector[y * 4 + x];
      if (sel & 1) hi |= 1u << (16 + i);
      if (sel & 2) hi |= 1u << i;
    }
  }
  // The block is two 32-bit words, the colour/mode word first and little-endian; the selector word follows it.
  std::memcpy(out, &lo, 4);
  std::memcpy(out + 4, &hi, 4);
}

// The EAC alpha half of an ETC2 RGBA block.
void write_alpha(const Pixel* px, uint8_t out[8]) {
  int mn = 255, mx = 0;
  for (int i = 0; i < 16; ++i) {
    mn = std::min(mn, px[i].a);
    mx = std::max(mx, px[i].a);
  }
  const int base = (mn + mx + 1) / 2;
  int best_err = 1 << 30, best_t = 0, best_mul = 0;
  int best_idx[16] = {};
  for (int t = 0; t < 16; ++t) {
    // the multiplier that spans the block's range with this table's outer modifiers
    const int range = std::max(1, kAlphaMods[t][7] - kAlphaMods[t][3]);
    const int ideal = (mx - mn) / range;   // how many multiples of this table's widest span the block covers
    for (int m = ideal - 1; m <= ideal + 1; ++m) {
      const int mul = std::clamp(m, 0, 15);
      const int mulv = mul == 0 ? 1 : mul * 8;   // the format's multiplier: index 0 means 1, the rest 8*k
      int err = 0, idx[16] = {};
      for (int i = 0; i < 16; ++i) {
        int bk = 0, bd = 1 << 30;
        for (int k = 0; k < 8; ++k) {
          const int v = base + (kAlphaMods[t][k] * mulv) / 8;
          const int d = std::clamp(v, 0, 255) - px[i].a;
          const int dd = d * d;
          if (dd < bd) {
            bd = dd;
            bk = k;
          }
        }
        idx[i] = bk;
        err += bd;
      }
      if (err < best_err) {
        best_err = err;
        best_t = t;
        best_mul = mul;
        std::memcpy(best_idx, idx, sizeof idx);
      }
    }
  }
  // The block, read as one big-endian 64-bit number: base in the top byte, the multiplier and table below it, then 48
  // bits of three-bit indices. Index group i - counted from the *bottom* - belongs to pixel (x, y) with i = x * 4 + y,
  // the same column-by-column order as the colour selectors.
  uint64_t block = (uint64_t)base << 56;
  block |= (uint64_t)((best_mul & 0xF) << 4 | (best_t & 0xF)) << 48;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      // The alpha indices run the other way round from the colour selectors: pixel (x, y) is group 15 - (4x + y).
      const int i = 15 - (x * 4 + y);
      block |= (uint64_t)(best_idx[y * 4 + x] & 7) << (3 * i);
    }
  }
  for (int i = 0; i < 8; ++i) out[i] = (uint8_t)(block >> (56 - 8 * i));
}

}  // namespace

bool block_is_opaque(const uint8_t* rgba, size_t stride) {
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      if (rgba[y * stride + x * 4 + 3] != 255) return false;
  return true;
}

void encode_rgb(const uint8_t* rgba, size_t stride, uint8_t out[8]) {
  Pixel px[16];
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) px[y * 4 + x] = load(rgba + y * stride + x * 4);

  // A block of one colour is common in this game's textures (and in every UI). One trial, no search.
  bool solid = true;
  for (int i = 1; i < 16 && solid; ++i)
    solid = px[i].r == px[0].r && px[i].g == px[0].g && px[i].b == px[0].b;
  if (solid) {
    Trial t;
    t.diff = 1;
    t.flip = 0;
    for (int ch = 0; ch < 3; ++ch) {
      const int v[3] = {px[0].r, px[0].g, px[0].b};
      t.base[0][ch] = std::clamp((v[ch] * 31 + 127) / 255, 0, 31);
      t.base[1][ch] = t.base[0][ch];
    }
    // the modifier that lands closest to the exact colour, same table and selector for both halves
    int best_t = 0, best_m = 0, best_d = 1 << 30;
    for (int tb = 0; tb < 8; ++tb) {
      for (int m = 0; m < 4; ++m) {
        int d = 0;
        for (int ch = 0; ch < 3; ++ch) {
          const int want = ch == 0 ? px[0].r : ch == 1 ? px[0].g : px[0].b;
          const int got = std::clamp(expand5(t.base[0][ch]) + kModifiers[tb][m], 0, 255);
          d += (got - want) * (got - want);
        }
        if (d < best_d) {
          best_d = d;
          best_t = tb;
          best_m = m;
        }
      }
    }
    t.table[0] = t.table[1] = best_t;   // both halves share one colour: the delta is zero
    t.selector[0] = t.selector[1] = t.selector[2] = t.selector[3] = best_m;
    for (int i = 0; i < 16; ++i) t.selector[i] = best_m;
    write_block(t, out);
    return;
  }

  Trial best;
  try_split(px, 0, best);
  try_split(px, 1, best);
  write_block(best, out);
}

namespace {
size_t block_count(uint32_t width, uint32_t height, uint32_t mips) {
  size_t blocks = 0;
  for (uint32_t mip = 0, w = width, h = height; mip < mips; ++mip) {
    blocks += size_t((w + 3) / 4) * ((h + 3) / 4);
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  return blocks;
}

// Walk the mip chain and hand each 4x4 block (padded at the edges) to `write_block`.
template <size_t BlockSize, typename Fn>
void for_each_block(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t mips, uint8_t* out, Fn write_block) {
  uint32_t w = width, h = height;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const size_t stride = size_t(w) * 4;
    for (uint32_t by = 0; by < h; by += 4) {
      for (uint32_t bx = 0; bx < w; bx += 4) {
        if (bx + 4 <= w && by + 4 <= h) {
          write_block(rgba + by * stride + bx * 4, stride, out);
        } else {
          // a partial block at the edge of an oddly sized (usually mipped-down) image: repeat the last row/column
          uint8_t edge[16 * 4];
          for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
              const uint32_t sx = bx + (uint32_t)std::min<int>(x, (int)w - 1 - (int)bx);
              const uint32_t sy = by + (uint32_t)std::min<int>(y, (int)h - 1 - (int)by);
              std::memcpy(&edge[(y * 4 + x) * 4], rgba + sy * stride + sx * 4, 4);
            }
          }
          write_block(edge, 16, out);
        }
        out += BlockSize;
      }
    }
    rgba += stride * h;
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
}
}  // namespace

size_t image_size(uint32_t width, uint32_t height, uint32_t mips) {
  return block_count(width, height, mips) * 8;
}

void encode_image(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t mips, uint8_t* out) {
  for_each_block<8>(rgba, width, height, mips, out, [](const uint8_t* block, size_t stride, uint8_t* dst) {
    encode_rgb(block, stride, dst);
  });
}

size_t image_size_rgba(uint32_t width, uint32_t height, uint32_t mips) {
  return block_count(width, height, mips) * 16;
}

void encode_image_rgba(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t mips, uint8_t* out) {
  for_each_block<16>(rgba, width, height, mips, out, [](const uint8_t* block, size_t stride, uint8_t* dst) {
    encode_rgba(block, stride, dst);
  });
}

void encode_rgba(const uint8_t* rgba, size_t stride, uint8_t out[16]) {
  Pixel px[16];
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) px[y * 4 + x] = load(rgba + y * stride + x * 4);
  write_alpha(px, out);
  encode_rgb(rgba, stride, out + 8);
}

}  // namespace aurora::gfx::etc2
