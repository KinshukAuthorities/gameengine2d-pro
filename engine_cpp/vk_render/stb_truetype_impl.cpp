/*
 * stb_truetype_impl.cpp — the ONE translation unit that compiles
 * stb_truetype.h's implementation (function bodies, not just declarations).
 *
 * stb_truetype.h is a single-header library: include it normally anywhere
 * you just need the declarations (vk_font_atlas.hpp does this), but exactly
 * one .cpp in the whole link must define STB_TRUETYPE_IMPLEMENTATION before
 * including it, or you get "multiply defined symbol" linker errors for
 * every stbtt_* function. Same pattern this codebase already uses for VMA
 * (see vk_mem_alloc_impl.cpp) — kept in its own tiny file so it's obvious
 * at a glance where the implementation lives.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"
