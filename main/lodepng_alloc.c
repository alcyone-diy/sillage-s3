#include "lodepng_alloc.h"

void* lodepng_malloc(size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return ptr;
}

void* lodepng_realloc(void* ptr, size_t new_size) {
    void* new_ptr = heap_caps_realloc(ptr, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!new_ptr) {
        new_ptr = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return new_ptr;
}

void lodepng_free(void* ptr) {
    heap_caps_free(ptr);
}
