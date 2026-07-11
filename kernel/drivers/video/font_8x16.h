#ifndef __FONT_8X16_H__
#define __FONT_8X16_H__

#include <stdint.h>


#define FONT_WIDTH 8
#define FONT_HEIGHT 16

// IBM VGA 8X16 font (public domain)
// 256 characters x 16 bytes each = 4096 bytes

extern const uint8_t font_8x16[4096];


#endif /* __FONT_8X16_H__ */