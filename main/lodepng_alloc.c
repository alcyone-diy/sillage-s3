#include "lodepng_alloc.h"

void* lodepng_malloc_mem(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void* lodepng_realloc_mem(void* ptr, size_t new_size) {
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lodepng_free_mem(void* ptr) {
    heap_caps_free(ptr);
}
