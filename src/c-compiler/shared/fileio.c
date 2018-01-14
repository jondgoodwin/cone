/** File I/O
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "fileio.h"
#include "memory.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/** Load a file into an allocated string, return pointer or NULL if not found */
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

/** Extract a filename only (no extension) from a path */
char *fileName(char *fn) {
	char *dotp;
	char *fnp = &fn[strlen(fn)-1];

	// Look backwards for '.' If not found, we are done
	while (fnp != fn && *fnp != '.' && *fnp != '/' && *fnp != '\\')
		--fnp;
	if (fnp == fn)
		return fn;
	if (*fnp == '/' || *fnp == '\\')
		return fnp + 1;

	// Look backwards for slash
	dotp = fnp;
	while (fnp != fn && *fnp != '/' && *fnp != '\\')
		--fnp;
	if (fnp != fn)
		++fnp;

	// Create string to hold filename and return
	return memAllocStr(fnp, dotp-fnp);
}

/** Concatenate folder, filename and extension into a path */
char *fileMakePath(char *dir, char *srcfn, char *ext) {
	char *outnm;
	if (dir == NULL)
		dir = "";
	outnm = memAllocStr(dir, strlen(dir) + strlen(srcfn) + strlen(ext) + 2);
	if (strlen(dir) && outnm[strlen(outnm) - 1] != '/' && outnm[strlen(outnm) - 1] != '\\')
		strcat(outnm, "/");
	strcat(outnm, srcfn);
	strcat(outnm, ".");
	strcat(outnm, ext);
	return outnm;
}
