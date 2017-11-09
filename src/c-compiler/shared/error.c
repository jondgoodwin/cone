/** Error Handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "error.h"
#include "../parser/lexer.h"
#include "ast.h"

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
	getchar();
	exit(exitcode);
}

// Send an error message to stderr
void errorMsg(AstNode *node, int code, const char *msg, ...) {
	va_list argptr;
	char *srcp;
	int pos, spaces;

	// Prefix for error message
	if (code<WarnCode) {
		errors++;
		fprintf(stderr, "Error %d: ", code);
	}
	else {
		warnings++;
		fprintf(stderr, "Warning %d: ", code);
	}

	// Do a formatted output of message, passing along all parms
	va_start(argptr, msg);
	vfprintf(stderr, msg, argptr);
	va_end(argptr);
	fputs("\n", stderr);

	// Reflect the source code line
	fputs(" --> ", stderr);
	srcp = node->linep;
	if (*srcp && *srcp!='\n')
		fputc(*srcp++, stderr);
	fputc('\n', stderr);

	// Depict where error message applies along with source file/pos info
	fprintf(stderr, "     ");
	pos = (spaces = node->srcp - node->linep) + 1;
	while (spaces--)
		fputc(' ', stderr);
	fprintf(stderr, "^--- %s:%d:%d\n", node->lexer->url, node->linenbr, (node->srcp - node->linep) + 1);
}

// Generate final message for a compile
void errorSummary() {
	if (errors > 0)
		errorExit(ExitError, "Unsuccessful compile: %d errors, %d warnings", errors, warnings);
	fprintf(stderr, "Compilation was successful. %d warnings detected\n", warnings);
}