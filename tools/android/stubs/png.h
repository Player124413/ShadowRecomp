// Minimal libpng surface, for local -fsyntax-only checks only (not used by any build).
#pragma once
#include <cstddef>
#include <cstdio>
#include <csetjmp>
typedef unsigned char png_byte;
typedef const unsigned char* png_const_bytep;
typedef unsigned char* png_bytep;
typedef size_t png_size_t;
struct png_struct;
struct png_info;
typedef png_struct* png_structp;
typedef png_info* png_infop;
typedef void* png_voidp;
typedef void (*png_error_ptr)(png_structp, const char*);
typedef void (*png_rw_ptr)(png_structp, png_bytep, png_size_t);
#define PNG_LIBPNG_VER_STRING "1.6.40"
#define PNG_INTERLACE_NONE 0
#define PNG_FILTER_TYPE_DEFAULT 0
#define PNG_FILTER_NONE 0
#define PNG_IMAGE_VERSION 1
#define PNG_FORMAT_RGBA 0x03
#define PNG_COLOR_TYPE_RGBA 6
#define PNG_COMPRESSION_TYPE_DEFAULT 0
struct png_image {
  unsigned version;
  unsigned width, height, format, flags;
  unsigned colormap_entries, warning_or_error;
  char message[64];
};
#define PNG_IMAGE_SIZE(image) (sizeof(png_byte) * 4 * (image).width * (image).height)
int png_image_begin_read_from_stdio(png_image*, FILE*);
int png_image_finish_read(png_image*, void*, void*, int, void*);
void png_image_free(png_image*);
png_structp png_create_write_struct(const char*, void*, png_error_ptr, png_error_ptr);
png_infop png_create_info_struct(png_structp);
void png_destroy_write_struct(png_structp*, png_infop*);
void png_set_write_fn(png_structp, void*, png_rw_ptr, void (*)(png_structp));
void png_set_filter(png_structp, int, int);
void png_set_compression_level(png_structp, int);
void png_set_IHDR(png_structp, png_infop, unsigned, unsigned, int, int, int, int, int);
void png_set_rows(png_structp, png_infop, png_bytep*);
void png_write_info(png_structp, png_infop);
void png_write_row(png_structp, const unsigned char*);
void png_write_end(png_structp, png_infop);
void* png_get_io_ptr(png_structp);
int png_setjmp(png_structp);
#define png_jmpbuf(p) 0
