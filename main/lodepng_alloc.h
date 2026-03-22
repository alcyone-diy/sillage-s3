#ifndef LODEPNG_ALLOC_H
#define LODEPNG_ALLOC_H

#include <stddef.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

// Define custom allocators for LodePNG to strictly use PSRAM.
// This prevents LodePNG from exhausting the ESP32-S3's internal RAM pool
// or relying on LVGL's internal lv_malloc mappings.
#define LODEPNG_NO_COMPILE_ALLOCATORS

#ifdef __cplusplus
extern "C" {
#endif

void* lodepng_malloc(size_t size);
void* lodepng_realloc(void* ptr, size_t new_size);
void lodepng_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif
