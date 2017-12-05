/** File i/o
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fileio_h
#define fileio_h

// Load a file into an allocated string, return pointer or NULL if not found
char *fileLoad(char *fn);

// Extract a filename only (no extension) from a path
char *fileName(char *fn);

// Concatenate folder, filename and extension into a path
char *fileMakePath(char *dir, char *srcfn, char *ext);

#endif