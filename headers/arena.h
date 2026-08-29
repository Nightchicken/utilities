#ifndef ARENA_H
#define ARENA_H
/**
 * @file arena.h
 * @brief Bump allocator over one malloc'd block.
 *
 * Allocation is a pointer bump, and there is no per-allocation free:
 * everything handed out lives until the whole arena is reset or destroyed.
 *
 * This suits code that allocates its long-lived buffers once at startup and
 * never again -- no ownership to track, no fragmentation, and teardown is a
 * single free() no matter how many pieces were carved out of it.
 *
 * Typical use:
 * @code
 *     Arena a = arena_create(1 << 20);   // 1 MiB backing block
 *     int* grid = (int*)arena_calloc(&a, 64 * 64 * sizeof(int));
 *     if (!grid) { ... out of arena space ... }
 *     arena_destroy(&a);                 // frees grid and everything else
 * @endcode
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief A fixed-capacity bump allocator.
 *
 * @var Arena::current
 *   Bump offset in bytes: range [0, current) is handed out, [current, capacity)
 *   is free. Because it is an offset rather than a pointer, saving it and
 *   restoring it later rewinds the arena to an earlier state for free.
 * @var Arena::capacity
 *   Total usable size of @ref Arena::data in bytes, or 0 if the backing malloc
 *   failed. A zero capacity is the single "this arena is unusable" signal.
 * @var Arena::data
 *   Backing block from malloc(), or NULL if the allocation failed. Owned by the
 *   arena; released by arena_destroy().
 */
typedef struct{
	size_t current;  // bytes used so far; also the offset of the next allocation
	size_t capacity; // total size of `data`, or 0 if the malloc failed
	unsigned char* data;
}Arena;

/**
 * @brief Alignment used by arena_alloc() and arena_calloc(), in bytes.
 *
 * 8 bytes satisfies every scalar type this project stores; the sizeof(void*)
 * branch keeps it correct on a platform with pointers wider than that.
 */
#define ARENA_DEFAULT_ALIGN (sizeof(void*) > 8 ? sizeof(void*) : 8)

/**
 * @brief Create an arena backed by a single malloc'd block.
 *
 * @param capacity Number of bytes to reserve up front. This is a hard ceiling:
 *                 the arena never grows, so size it for the peak live total of
 *                 every allocation plus alignment padding between them.
 * @return An Arena by value. On allocation failure the returned arena has
 *         `data == NULL` and `capacity == 0` rather than reporting the error
 *         separately, so every later arena_alloc() simply returns NULL and the
 *         caller has one failure path instead of two.
 *
 * @note Pass the result to arena_destroy() when done, even on failure.
 */
Arena arena_create(size_t capacity){
	Arena a;
	a.current = 0;
	a.capacity = capacity;
	a.data = (unsigned char*)malloc(capacity);
	if (!a.data) a.capacity = 0;
	return a;
}

/**
 * @brief Round a value up to the next multiple of an alignment.
 *
 * @param n     Value to round up, in bytes. Typically an offset into the arena.
 * @param align Alignment in bytes. Must be a power of two -- the bit trick
 *              below silently produces garbage otherwise. Every caller here
 *              satisfies that.
 * @return The smallest multiple of @p align that is >= @p n.
 *
 * @warning Overflows if @p n is within @p align of SIZE_MAX; arena offsets are
 *          nowhere near that, so no check is made.
 */
size_t arena_align_up(size_t n, size_t align){
	return (n + (align - 1)) & ~(align - 1);
}

/**
 * @brief Carve out a block at a caller-chosen alignment.
 *
 * @param self  Arena to allocate from. Must not be NULL; a zero-capacity arena
 *              is fine and always fails the fit check.
 * @param size  Number of bytes requested. A size of 0 returns a valid,
 *              non-NULL pointer and consumes only alignment padding.
 * @param align Required alignment of the returned pointer, in bytes. Must be a
 *              power of two (see arena_align_up()).
 * @return Pointer to @p size uninitialised bytes aligned to @p align, or NULL
 *         if the request does not fit in the remaining capacity.
 *
 * @note On failure `current` is left untouched, so a failed allocation cannot
 *       corrupt the arena or partially consume it.
 * @note The returned pointer stays valid until arena_reset() or
 *       arena_destroy(); it must never be passed to free().
 */
void* arena_alloc_aligned(Arena* self, size_t size, size_t align){
	size_t offset = arena_align_up(self->current, align);
	if (offset + size > self->capacity) return NULL;
	void* ptr = self->data + offset;
	self->current = offset + size;
	return ptr;
}

/**
 * @brief Allocate uninitialised bytes at the default alignment.
 *
 * @param self Arena to allocate from. Must not be NULL.
 * @param size Number of bytes requested.
 * @return Pointer to @p size bytes aligned to ::ARENA_DEFAULT_ALIGN, or NULL if
 *         the request does not fit.
 *
 * @warning The memory is uninitialised -- use arena_calloc() if you need zeros.
 */
void* arena_alloc(Arena* self, size_t size){
	return arena_alloc_aligned(self, size, ARENA_DEFAULT_ALIGN);
}

/**
 * @brief Allocate zeroed bytes at the default alignment.
 *
 * @param self Arena to allocate from. Must not be NULL.
 * @param size Number of bytes requested, all of which are set to 0.
 * @return Pointer to @p size zeroed bytes, or NULL if the request does not fit.
 *
 * @see arena_alloc()
 */
void* arena_calloc(Arena* self, size_t size){
	void* ptr = arena_alloc(self, size);
	if (ptr) memset(ptr, 0, size);
	return ptr;
}

/**
 * @brief Hand the whole block back for reuse without freeing it.
 *
 * @param self Arena to rewind. Its `data` and `capacity` are unchanged; only
 *             the bump offset returns to 0.
 *
 * @warning Every pointer previously returned by this arena is dangling
 *          afterwards. Do not call this while anything still holds one --
 *          buffers meant to live for the lifetime of the program in particular.
 */
void arena_reset(Arena* self){
	self->current = 0;
}

/**
 * @brief Release the backing block.
 *
 * @param self Arena to tear down. Must not be NULL. Afterwards it is left in
 *             the same state as a zero-capacity arena (`data == NULL`,
 *             `current == 0`, `capacity == 0`), so a double destroy is
 *             harmless and the struct can be reassigned from arena_create().
 *
 * @warning Invalidates every pointer the arena ever returned.
 */
void arena_destroy(Arena* self){
	free(self->data);
	self->data = NULL;
	self->current = 0;
	self->capacity = 0;
}

#endif

/*
 * MIT License
 * Copyright (c) 2026 Matthew Kabat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
