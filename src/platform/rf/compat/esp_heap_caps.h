/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * esp_heap_caps shim. On Linux there is no PSRAM/heap-cap split; report plenty
 * of free memory so the engine's low-memory guard never trips, and route any
 * capability-malloc to the ordinary allocator.
 */
#pragma once
#include <cstddef>
#include <cstdlib>

#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM 0
#endif

inline size_t heap_caps_get_free_size(unsigned) { return (size_t)64 * 1024 * 1024; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return (size_t)16 * 1024 * 1024; }
inline void *heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
inline void *heap_caps_calloc(size_t c, size_t s, unsigned) { return std::calloc(c, s); }
inline void heap_caps_free(void *p) { std::free(p); }
