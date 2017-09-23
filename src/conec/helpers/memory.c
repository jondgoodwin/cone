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

// Public globals: Arena size configuration values
size_t gMemBlkArenaSize = 256 * 4096;
size_t gMemStrArenaSize = 128 * 4096;

// Private globals: memory allocation arena bookkeeping
static void *gMemBlkArenaPos = NULL;
static size_t gMemBlkArenaLeft = 0;
static void *gMemStrArenaPos = NULL;
static size_t gMemStrArenaLeft = 0;

/** Allocate memory for a block, aligned to a 16-byte boundary */
void *memAllocBlk(size_t size) {
	void *memp;

	// Align to 16-byte boundary
	size = (size + 15) & ~15;

	// Return next bite out of arena, if it fits
	if (size <= gMemBlkArenaLeft) {
		gMemBlkArenaLeft -= size;
		memp = gMemBlkArenaPos;
		gMemBlkArenaPos = (char*)gMemBlkArenaPos + size;
		return memp;
	}

	// Return a newly allocated area, if bigger than arena can hold
	if (size > gMemBlkArenaSize) {
		memp = malloc(size);
		if (memp==NULL) {
			puts("Error: Out of memory");
			exit(1);
		}
		return memp;
	}

	// Allocate a new Arena and return next bite out of it
	gMemBlkArenaPos = malloc(gMemBlkArenaSize);
	if (gMemBlkArenaPos==NULL) {
		puts("Error: Out of memory");
		exit(1);
	}
	gMemBlkArenaLeft = gMemBlkArenaSize - size;
	memp = gMemBlkArenaPos;
	gMemBlkArenaPos = (char*)gMemBlkArenaPos + size;
	return memp;
}

/** Allocate memory for a string and copy contents over, if not NULL
 * Allocates extra byte for string-ending 0, appending it to copied string */
char *memAllocStr(char *str, size_t size) {
	void *strp;

	// Give it room for C-string null terminator
	size += 1;

	// Return next bite out of arena, if it fits
	if (size <= gMemStrArenaLeft) {
		gMemStrArenaLeft -= size;
		strp = gMemStrArenaPos;
		gMemStrArenaPos = (char*)gMemStrArenaPos + size;
	}

	// Return a newly allocated area, if bigger than arena can hold
	else if (size > gMemStrArenaSize) {
		strp = malloc(size);
		if (strp==NULL) {
			puts("Error: Out of memory");
			exit(1);
		}
	}

	// Allocate a new Arena and return next bite out of it
	else {
		gMemStrArenaPos = malloc(gMemStrArenaSize);
		if (gMemStrArenaPos==NULL) {
			puts("Error: Out of memory");
			exit(1);
		}
		gMemStrArenaLeft = gMemStrArenaSize - size;
		strp = gMemStrArenaPos;
		gMemStrArenaPos = (char*)gMemStrArenaPos + size;
	}

	// Copy string contents into it
	if (str) {
		strncpy((char*)strp, str, --size);
		*((char*)strp+size) = '\0';
	}
	return (char*) strp;
}
