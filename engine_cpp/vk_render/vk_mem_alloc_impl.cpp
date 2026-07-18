// vk_mem_alloc_impl.cpp — single translation unit that compiles the actual
// Vulkan Memory Allocator implementation.
//
// vk_mem_alloc.h is header-only: every other file includes it purely for
// declarations. Exactly one .cpp in the whole link must define
// VMA_IMPLEMENTATION before including the header so the function bodies
// (vmaCreateBuffer, vmaDestroyImage, vmaCreateAllocator, ...) actually get
// emitted somewhere — otherwise every call site link-fails with
// "undefined reference to vma*". This is that one file.
//
// Must use the same VMA_VULKAN_VERSION as vk_context.hpp so both
// translation units agree on the struct layouts being compiled against.

#define VMA_VULKAN_VERSION 1001000
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
