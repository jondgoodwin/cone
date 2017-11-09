/** Error Handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int errors = 0;
int warnings = 0;

// Send an error message to stderr
void errorExit(int exitcode, const char *msg, ...) {
	// Do a formatted output, passing along all parms
	va_list argptr;
	va_start(argptr, msg);
	vfprintf(stderr, msg, argptr);
	va_end(argptr);
	fputs("\n", stderr);

	// Exit with return code
	exit(exitcode);
}

// Generate final message for a compile
void errorSummary() {
	if (errors > 0)
		errorExit(ExitError, "Unsuccessful compile: %d errors, %d warnings\n", errors, warnings);
	fprintf(stderr, "Compilation was successful. %d warnings detected\n", warnings);
}