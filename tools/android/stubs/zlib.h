#pragma once
#include <cstddef>
typedef unsigned char Bytef;
typedef unsigned long uLong;
typedef unsigned long uLongf;
/* Return codes the runtime checks (zlib.h's own values). */
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_NEED_DICT 2
#define Z_ERRNO (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR (-3)
#define Z_MEM_ERROR (-4)
#define Z_BUF_ERROR (-5)
#define Z_VERSION_ERROR (-6)
#define Z_DEFAULT_COMPRESSION (-1)
int uncompress(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen);
unsigned long crc32(unsigned long crc, const unsigned char* buf, unsigned int len);
int compress2(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen, int level);
