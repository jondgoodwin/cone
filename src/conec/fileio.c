/** File I/O
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "memory.h"

#include <stdio.h>

// Load a file into an allocated string, return pointer or NULL if not found
char *fileLoad(char *fn) {
	FILE *file;
	size_t filesize;
	char *filestr;

	// Open the file - return null on failure
	if (!(file = fopen(fn, "rb")))
		return NULL;

	// Determine the file length (so we can accurately allocate memory)
	fseek(file, 0, SEEK_END);
	filesize=ftell(file);
	fseek(file, 0, SEEK_SET);

	// Load the data into an allocated string buffer and close file
	filestr = memAllocStr(NULL, filesize);
	fread(filestr, 1, filesize, file);
	filestr[filesize]='\0';
	fclose(file);
	return filestr;
}