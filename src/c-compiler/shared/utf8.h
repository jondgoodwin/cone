/** UTF-8 helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef utf8_h
#define utf8_h

#include <stdint.h>

// How many bytes the character at src occupies: the length its lead byte
// declares, but only when that many continuation bytes actually follow.
// Anything malformed -- a continuation byte standing alone, a byte no character
// may start with, or a lead byte whose sequence is cut short -- counts as one,
// so a caller reports it and resumes at the byte after. Zero at end of source.
int utf8ByteSkip(const char *src);

// Non-zero if src starts a well-formed character of more than one byte
int utf8IsMultibyte(const char *src);

uint32_t utf8GetCode(const char *src);
int utf8IsLetter(const char* srcp);

#endif
