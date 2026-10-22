// ETC2 block encoder - see etc2_encode.hpp for why and for the block layout.
//
// The search, per block:
//   * a block of one colour is written directly (very common in this game's art);
//   * for each of the two sub-block splits (flip 0: left/right, flip 1: top/bottom) each sub-block's base colour is the
//     mean of its pixels, refined twice against the modifiers the table offers, and the two best of the eight modifier
//     tables are kept;
//   * each of those is then quantised to the individual mode (4-bit base, no delta) and to the differential mode (5-bit
//     base, 3-bit delta) and the cheapest of the four is written out.
//
// The selector search is what makes this fast enough to run while the game streams textures. For one pixel and one
// table, with d = base - pixel, S1 = sum(d), S2 = sum(d*d), the error of modifier m is
//     S2 - (2*m*|S1| - 3*m*m)
// so the best of the four modifiers is the larger magnitude the table has when it "gains" more, which is a handful of
// integer operations per pixel per table instead of a squared distance in floating point per modifier. The earlier
// version of this file took 16 us per 4x4 block; this one takes about 2, which is the difference between a texture
// upload being invisible and a visible hitch (a movie re-uploads its frame every frame).
#include "etc2_encode.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

namespace aurora::gfx::etc2 {
namespace {

// The eight ETC modifier tables, in 8-bit units (the format's own constants). [0]/[1] are the positive magnitudes,
// [2]/[3] the negatives; a pixel's selector is the index into this row.
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

// A 5-bit / 4-bit colour channel, expanded the way the hardware does it.
inline int expand5(int v) { return (v << 3) | (v >> 2); }
inline int expand4(int v) { return (v << 4) | v; }

inline int clamp_channel(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

struct Pixel {
  int r, g, b, a;
};

inline Pixel load(const uint8_t* p) { return Pixel{p[0], p[1], p[2], p[3]}; }

// Selectors for one sub-block against one base colour and one table; returns the squared error.
//
// For a pixel, d = base - pixel, S1 = d.r + d.g + d.b, S2 = |d|^2, and a modifier m gives |d + m|^2 = S2 + 2*m*S1 +
// 3*m*m. Writing that as S2 - gain(m) with gain(m) = -2*m*S1 - 3*m*m, the best modifier is the one with the largest
// gain; with S1 < 0 (pixels brighter than the base) that is the positive magnitude, otherwise the negative one.
inline int eval_base(const Pixel* px, int count, int table, const int base[3], int sel[8]) {
  const int a = kModifiers[table][0];
  const int b = kModifiers[table][1];
  const int cost_a = 3 * a * a;   // gain(m) = 2*m*|S1| - 3*m*m
  const int cost_b = 3 * b * b;
  int total = 0;
  for (int k = 0; k < count; ++k) {
    const Pixel& p = px[k];
    const int dr = base[0] - p.r;
    const int dg = base[1] - p.g;
    const int db = base[2] - p.b;
    const int s1 = dr + dg + db;
    const int s2 = dr * dr + dg * dg + db * db;
    const int abs1 = s1 < 0 ? -s1 : s1;
    const int ga = 2 * a * abs1 - cost_a;
    const int gb = 2 * b * abs1 - cost_b;
    // branch-free on purpose: texture blocks are exactly the data where a branch on the pixel is unpredictable
    const int bigger = ga > gb ? 0 : 1;   // 1: the table's larger magnitude wins
    const int dark = s1 > 0 ? 1 : 0;      // 1: the pixels are darker than the base, so a negative modifier
    sel[k] = 2 * dark + bigger;
    total += s2 - (ga > gb ? ga : gb);
  }
  return total;
}

// The base that best fits the given selectors: the mean of the pixels minus their modifiers.
inline void refine_base(const Pixel* px, int count, int table, const int sel[8], int base[3]) {
  int sum[3] = {0, 0, 0};
  for (int k = 0; k < count; ++k) {
    const Pixel& p = px[k];
    const int m = kModifiers[table][sel[k]];
    sum[0] += p.r - m;
    sum[1] += p.g - m;
    sum[2] += p.b - m;
  }
  const int half = count / 2;
  for (int ch = 0; ch < 3; ++ch) {
    const int v = sum[ch] >= 0 ? (sum[ch] + half) / count : -((-sum[ch] + half) / count);
    base[ch] = clamp_channel(v);
  }
}

// One sub-block: up to two candidate (table, 8-bit base) pairs, best first.
struct Candidates {
  int table[2] = {0, 0};
  int base[2][3] = {};
  int error[2] = {INT_MAX, INT_MAX};
  int count = 0;
};

// Fits a sub-block: the mean base against every table, then refinement rounds on the best two tables.
void fit_subblock(const Pixel* px, int count, Candidates& out) {
  int sum[3] = {0, 0, 0};
  for (int k = 0; k < count; ++k) {
    const Pixel& p = px[k];
    sum[0] += p.r;
    sum[1] += p.g;
    sum[2] += p.b;
  }
  const int half = count / 2;
  const int mean[3] = {clamp_channel((sum[0] + half) / count), clamp_channel((sum[1] + half) / count),
                       clamp_channel((sum[2] + half) / count)};

  int order[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  int err[8];
  int sel[8];
  for (int t = 0; t < 8; ++t) {
    err[t] = eval_base(px, count, t, mean, sel);
  }
  // the two best tables (a selection sort, but only two passes)
  for (int i = 0; i < 2; ++i) {
    int bestI = i;
    for (int j = i + 1; j < 8; ++j) {
      if (err[order[j]] < err[order[bestI]]) bestI = j;
    }
    std::swap(order[i], order[bestI]);
  }

  out.count = 2;
  for (int c = 0; c < 2; ++c) {
    const int t = order[c];
    int base[3] = {mean[0], mean[1], mean[2]};
    int sel_buf[8];
    int e = eval_base(px, count, t, base, sel_buf);
    // The second candidate is a cheap safety net for the first (a block whose two best tables are close): it gets the
    // mean only, without the refinement rounds.
    for (int iter = 0; iter < (c == 0 ? 2 : 0); ++iter) {
      int refined[3];
      refine_base(px, count, t, sel_buf, refined);
      int sel_buf2[8];
      const int e2 = eval_base(px, count, t, refined, sel_buf2);
      if (e2 < e) {
        e = e2;
        std::memcpy(base, refined, sizeof base);
        std::memcpy(sel_buf, sel_buf2, sizeof sel_buf);
      }
    }
    out.table[c] = t;
    std::memcpy(out.base[c], base, sizeof base);
    out.error[c] = e;
  }
}

// One sub-block's answer for one base-colour mode: which table, the selectors, the stored base values, and how wrong.
struct Sub {
  int table = 0;
  int selector[8] = {};   // 0..3, per pixel of the sub-block
  int error = INT_MAX;
  int quant[3] = {};      // as the mode stores them (4- or 5-bit)
};

// Quantises the candidates to a mode's precision and picks the cheaper one.
Sub apply_mode(const Pixel* px, int count, const Candidates& cands, int quant_bits) {
  const int levels = quant_bits == 5 ? 31 : 15;
  Sub best;
  for (int c = 0; c < cands.count; ++c) {
    Sub s;
    s.table = cands.table[c];
    int base[3];
    for (int ch = 0; ch < 3; ++ch) {
      const int q = std::clamp((cands.base[c][ch] * levels + 127) / 255, 0, levels);
      s.quant[ch] = q;
      base[ch] = quant_bits == 5 ? expand5(q) : expand4(q);
    }
    int sel[8];
    s.error = eval_base(px, count, s.table, base, sel);
    for (int k = 0; k < count; ++k) s.selector[k] = sel[k];
    if (s.error < best.error) best = s;
  }
  return best;
}

struct Trial {
  int error = INT_MAX;
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
  Pixel subpx[2][8];
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const int i = y * 4 + x;
      const int s = flip ? (y >= 2) : (x >= 2);   // flip 0: left/right halves, flip 1: top/bottom
      subpx[s][n[s]] = px[i];
      sub[s][n[s]++] = i;
    }
  }

  Candidates cands[2];
  for (int s = 0; s < 2; ++s) {
    fit_subblock(subpx[s], n[s], cands[s]);
  }

  // individual mode: a 4-bit base per sub-block, the halves independent
  {
    Trial t;
    t.flip = flip;
    t.diff = 0;
    int total = 0;
    for (int s = 0; s < 2; ++s) {
      const Sub r = apply_mode(subpx[s], n[s], cands[s], 4);
      for (int ch = 0; ch < 3; ++ch) t.base[s][ch] = r.quant[ch];
      t.table[s] = r.table;
      for (int k = 0; k < n[s]; ++k) t.selector[sub[s][k]] = r.selector[k];
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
      const Sub r = apply_mode(subpx[s], n[s], cands[s], 5);
      for (int ch = 0; ch < 3; ++ch) t.base[s][ch] = r.quant[ch];
      t.table[s] = r.table;
      for (int k = 0; k < n[s]; ++k) t.selector[sub[s][k]] = r.selector[k];
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
  const int spread = mx - mn;

  // The indices for one table at one multiplier: for every pixel, the nearest of the eight values the table can make.
  // The eight are computed once per table/multiplier instead of once per pixel.
  auto evaluate = [&](int t, int mul, int idx[16]) -> int {
    const int mulv = mul == 0 ? 1 : mul * 8;
    int value[8];
    for (int k = 0; k < 8; ++k) value[k] = clamp_channel(base + (kAlphaMods[t][k] * mulv) / 8);
    int err = 0;
    for (int i = 0; i < 16; ++i) {
      const int a = px[i].a;
      // distance and index packed into one integer (the index in the low three bits, the distance above it), so the
      // search is a minimum over eight values with no branch on which one wins
      int best = INT_MAX;
      for (int k = 0; k < 8; ++k) {
        const int d = value[k] - a;
        const int packed = ((d < 0 ? -d : d) << 3) | k;
        best = best < packed ? best : packed;
      }
      idx[i] = best & 7;
      const int dd = best >> 3;
      err += dd * dd;
    }
    return err;
  };

  // Every table's multiplier that spans the block's range, then the two best tables also get their neighbours.
  int err_of[16];
  int mul_of[16];
  int idx_of[16][16];
  for (int t = 0; t < 16; ++t) {
    const int range = std::max(1, kAlphaMods[t][7] - kAlphaMods[t][3]);
    mul_of[t] = std::clamp((spread + range / 2) / range, 0, 15);
    err_of[t] = evaluate(t, mul_of[t], idx_of[t]);
  }
  int best_err = INT_MAX, best_t = 0, best_mul = 0;
  int best_idx[16] = {};
  auto consider = [&](int t, int mul, const int idx[16], int err) {
    if (err < best_err) {
      best_err = err;
      best_t = t;
      best_mul = mul;
      std::memcpy(best_idx, idx, sizeof best_idx);
    }
  };
  for (int t = 0; t < 16; ++t) consider(t, mul_of[t], idx_of[t], err_of[t]);
  int first = 0, second = -1;
  for (int t = 1; t < 16; ++t) {
    if (err_of[t] < err_of[first]) {
      second = first;
      first = t;
    } else if (second < 0 || err_of[t] < err_of[second]) {
      second = t;
    }
  }
  for (int t : {first, second}) {
    if (t < 0) continue;
    for (int dm = -1; dm <= 1; dm += 2) {
      const int m = std::clamp(mul_of[t] + dm, 0, 15);
      if (m == mul_of[t]) continue;
      int idx[16];
      const int err = evaluate(t, m, idx);
      consider(t, m, idx, err);
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
    int best_t = 0, best_m = 0, best_d = INT_MAX;
    for (int tb = 0; tb < 8; ++tb) {
      for (int m = 0; m < 4; ++m) {
        int d = 0;
        for (int ch = 0; ch < 3; ++ch) {
          const int want = ch == 0 ? px[0].r : ch == 1 ? px[0].g : px[0].b;
          const int got = clamp_channel(expand5(t.base[0][ch]) + kModifiers[tb][m]);
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
