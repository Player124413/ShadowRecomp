#pragma once
#include <cstddef>
typedef unsigned char Bytef;
typedef unsigned long uLong;
typedef unsigned long uLongf;
int uncompress(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen);
unsigned long crc32(unsigned long crc, const unsigned char* buf, unsigned int len);
int compress2(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen, int level);
