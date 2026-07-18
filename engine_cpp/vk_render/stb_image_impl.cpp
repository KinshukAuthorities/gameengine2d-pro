/*
 * stb_image_impl.cpp — the ONE translation unit that compiles
 * stb_image.h's implementation (function bodies, not just declarations).
 *
 * Same single-header-library pattern as stb_truetype_impl.cpp /
 * vk_mem_alloc_impl.cpp: stb_image.h's declarations are included normally
 * wherever needed (texture_system_vk.hpp does this), but exactly one .cpp
 * in the whole link must define STB_IMAGE_IMPLEMENTATION before including
 * it, or every stbi_* function is multiply defined at link time.
 *
 * This replaces the previous texture-decode chain (SDL2_image when
 * installed -> Windows WIC COM API -> SDL_LoadBMP-only fallback) with a
 * single, cross-platform, dependency-free decoder: PNG, JPEG, BMP, GIF,
 * TGA, and PSD all work the same way on every platform, with no install
 * step and no Windows-only code path to maintain.
 */

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
