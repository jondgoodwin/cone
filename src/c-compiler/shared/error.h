/** Error handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef error_h
#define error_h

// Exit error codes
enum ErrorCode {
	ExitError,	// Program fails to compile due to caught errors
	ExitNF,		// Could not find specified source files
	ExitMem,	// Out of memory
	ExitOpts	// Invalid compiler options
};

// Send an error message to stderr
void errorExit(int exitcode, const char *msg, ...);
void errorSummary();

#endif