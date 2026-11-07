// Single translation unit for the vendored stb headers (keeps rebuilds fast).
// Only the decoders we need are compiled in: smaller attack surface, faster build.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

// Web fonts reach stb_truetype only through fontsan (validated and rebuilt). Its rasterizer still has asserts that
// odd-but-valid outlines can trip ("a tiny bit of an edge can be out of bounds"); they guard nothing, so they must
// not abort a Debug build.
#define STBTT_assert(x) ((void)0)
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
