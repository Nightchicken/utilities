#ifndef ARENA_H
#define ARENA_H
// Bump allocator over one malloc'd block. Allocation is a pointer bump, and
// there is no per-allocation free: everything handed out lives until the whole
// arena is reset or destroyed.
//
// This suits a game that allocates its screen planes once at startup and never
// again -- no ownership to track, no fragmentation, and teardown is a single
// free() no matter how many pieces were carved out of it.
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// `current` is the bump offset: bytes [0, current) are handed out, the rest is
// free. Because it is an offset rather than a pointer, saving and restoring it
// (see screen_create) rewinds the arena to an earlier state for free.
typedef struct{
	size_t current;  // bytes used so far; also the offset of the next allocation
	size_t capacity; // total size of `data`, or 0 if the malloc failed
	unsigned char* data;
}Arena;

// Alignment used by arena_alloc(). 8 bytes satisfies every scalar type this
// project stores; the sizeof(void*) branch keeps it correct on a platform with
// pointers wider than that.
#define ARENA_DEFAULT_ALIGN (sizeof(void*) > 8 ? sizeof(void*) : 8)

// Reserve `capacity` bytes up front. On allocation failure the capacity is
// zeroed rather than reported separately, so every later arena_alloc() simply
// returns NULL and the caller has one failure path instead of two.
Arena arena_create(size_t capacity){
	Arena a;
	a.current = 0;
	a.capacity = capacity;
	a.data = (unsigned char*)malloc(capacity);
	if (!a.data) a.capacity = 0;
	return a;
}

// Round `n` up to the next multiple of `align`. The bit trick requires align to
// be a power of two, which every caller here satisfies.
size_t arena_align_up(size_t n, size_t align){
	return (n + (align - 1)) & ~(align - 1);
}

// Carve out `size` bytes starting at an `align`-aligned offset. Returns NULL
// when the request does not fit, leaving `current` untouched so a failed
// allocation cannot corrupt the arena or partially consume it.
void* arena_alloc_aligned(Arena* self, size_t size, size_t align){
	size_t offset = arena_align_up(self->current, align);
	if (offset + size > self->capacity) return NULL;
	void* ptr = self->data + offset;
	self->current = offset + size;
	return ptr;
}

// Allocate `size` bytes at the default alignment. Returns NULL if it does not
// fit; the memory is uninitialised.
void* arena_alloc(Arena* self, size_t size){
	return arena_alloc_aligned(self, size, ARENA_DEFAULT_ALIGN);
}

// As arena_alloc(), but zeroed.
void* arena_calloc(Arena* self, size_t size){
	void* ptr = arena_alloc(self, size);
	if (ptr) memset(ptr, 0, size);
	return ptr;
}

// Hand the whole block back for reuse without freeing it. Every pointer
// previously returned by the arena is dangling afterwards, so this must not be
// called while anything still holds one -- the Screen planes in particular live
// for the lifetime of the program.
void arena_reset(Arena* self){
	self->current = 0;
}

// Release the backing block. The struct is left in the same state as a
// zero-capacity arena, so a double destroy is harmless.
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
