/** Memory management
 * @file
 *
 * The compiler's memory management is deliberately leaky for high performance.
 * Allocation is done via bump pointer within very large arenas allocated from the heap
 * Nothing is ever freed.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../helpers.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MEM_ARENA_SIZE 256 * 4096 - 64
#define STR_ARENA_SIZE 128 * 4096 - 64

// Global variables for memory allocation arenas
static void *memArenaPos = NULL;
static size_t memArenaSize = 0;
static void *strArenaPos = NULL;
static size_t strArenaSize = 0;

/** Allocate memory for a block, aligned to a 16-byte boundary */
void *memAllocBlk(size_t size) {
	void *memp;

	// Align to 16-byte boundary
	size = (size + 15) & ~15;

	// Return next bite out of arena, if it fits
	if (size <= memArenaSize) {
		memArenaSize -= size;
		memp = memArenaPos;
		memArenaPos = (char*)memArenaPos + size;
		return memp;
	}

	// Return a newly allocated area, if bigger than arena can hold
	if (size > MEM_ARENA_SIZE) {
		memp = malloc(size);
		if (memp==NULL) {
			puts("Error: Out of memory");
			exit(1);
		}
		return memp;
	}

	// Allocate a new Arena and return next bite out of it
	memArenaPos = malloc(MEM_ARENA_SIZE);
	if (memArenaPos==NULL) {
		puts("Error: Out of memory");
		exit(1);
	}
	memArenaSize = MEM_ARENA_SIZE - size;
	memp = memArenaPos;
	memArenaPos = (char*)memArenaPos + size;
	return memp;
}

/** Allocate memory for a string and copy contents over, if not NULL
 * Allocates extra byte for string-ending 0, appending it to copied string */
char *memAllocStr(char *str, size_t size) {
	void *strp;

	// Give it room for C-string null terminator
	size += 1;

	// Return next bite out of arena, if it fits
	if (size <= strArenaSize) {
		strArenaSize -= size;
		strp = strArenaPos;
		strArenaPos = (char*)strArenaPos + size;
	}

	// Return a newly allocated area, if bigger than arena can hold
	else if (size > STR_ARENA_SIZE) {
		strp = malloc(size);
		if (strp==NULL) {
			puts("Error: Out of memory");
			exit(1);
		}
	}

	// Allocate a new Arena and return next bite out of it
	else {
		strArenaPos = malloc(STR_ARENA_SIZE);
		if (strArenaPos==NULL) {
			puts("Error: Out of memory");
			exit(1);
		}
		strArenaSize = STR_ARENA_SIZE - size;
		strp = strArenaPos;
		strArenaPos = (char*)strArenaPos + size;
	}

	// Copy string contents into it
	if (str) {
		strncpy((char*)strp, str, --size);
		*((char*)strp+size) = '\0';
	}
	return (char*) strp;
}
