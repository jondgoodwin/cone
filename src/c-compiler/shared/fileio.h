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

#endif