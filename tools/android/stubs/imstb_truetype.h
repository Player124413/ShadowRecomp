// Minimal stb_truetype surface, for local -fsyntax-only checks only (not used by any build).
#pragma once
#define STBTT_STATIC
struct stbtt_fontinfo { void* userdata; unsigned char* data; int cursor; };
struct stbtt_bakedchar { unsigned short x0, y0, x1, y1; float xoff, yoff, xadvance; };
struct stbtt_aligned_quad { float x0, y0, s0, t0, x1, y1, s1, t1; };
int stbtt_GetFontOffsetForIndex(const unsigned char*, int);
int stbtt_InitFont(stbtt_fontinfo*, const unsigned char*, int);
float stbtt_ScaleForPixelHeight(const stbtt_fontinfo*, float);
float stbtt_ScaleForMappingEmToPixels(const stbtt_fontinfo*, float);
void stbtt_GetFontVMetrics(const stbtt_fontinfo*, int*, int*, int*);
int stbtt_FindGlyphIndex(const stbtt_fontinfo*, int);
void stbtt_GetGlyphHMetrics(const stbtt_fontinfo*, int, int*, int*);
int stbtt_GetGlyphKernAdvance(const stbtt_fontinfo*, int, int);
void stbtt_GetGlyphBitmapBox(const stbtt_fontinfo*, int, float, float, int*, int*, int*, int*);
int stbtt_GetGlyphBox(const stbtt_fontinfo*, int, int*, int*, int*, int*);
void stbtt_GetCodepointHMetrics(const stbtt_fontinfo*, int, int*, int*);
int stbtt_GetCodepointKernAdvance(const stbtt_fontinfo*, int, int);
int stbtt_GetCodepointBox(const stbtt_fontinfo*, int, int*, int*, int*, int*);
unsigned char* stbtt_GetCodepointBitmap(const stbtt_fontinfo*, float, float, int, int*, int*, int*, int*);
unsigned char* stbtt_GetGlyphBitmapSubpixel(const stbtt_fontinfo*, float, float, float, float, int, int*, int*, int*, int*);
void stbtt_FreeBitmap(unsigned char*, void*);
