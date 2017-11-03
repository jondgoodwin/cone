/** Error Handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "error.h"

#include <stdio.h>
#include <stdarg.h>

/** Send an error message to stderr */
void errorExit(const char *msg, ...) {
	// Do a formatted output, passing along all parms
	va_list argptr;
	va_start(argptr, msg);
	vfprintf(stderr, msg, argptr);
	va_end(argptr);
	fputs("\n", stderr);
}