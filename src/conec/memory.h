/** Memory management
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef memory_h
#define memory_h

#include <stdio.h>

// Allocate a structure, aligned to a 16-byte boundary
void *memAllocBlk(size_t size);

// Allocate memory for a string and copy contents over, if not NULL
char *memAllocStr(char *str, size_t size);

#endif