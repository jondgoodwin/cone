/** Error handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef error_h
#define error_h

typedef struct AstNode AstNode;	// ../ast/ast.h

// Exit error codes
enum ErrorCode {
	// Terminating errors
	ExitError,	// Program fails to compile due to caught errors
	ExitNF,		// Could not find specified source files
	ExitMem,	// Out of memory
	ExitOpts,	// Invalid compiler options

	// Non-terminating errors
	ErrorCode = 1000,
	ErrorBadTok,	// Bad character starting an unknown token
	ErrorNoSemi,	// Missing semicolon
	ErrorNoRCurly,	// Missing right curly brace
	ErrorBadTerm,   // Invalid term - something other than var, lit, etc.
	ErrorBadGloStmt, // Invalid global area type, var or function statement
	ErrorFnDupImpl, // Function already has another implementation
	ErrorNoLParen,   // Expected left parenthesis not found
	ErrorNoRParen,	 // Expected right parenthesis not found
	ErrorDupType,	// May not specify duplicate types

	// Warnings
	WarnCode = 3000,
};

// Send an error message to stderr
void errorExit(int exitcode, const char *msg, ...);
void errorMsgNode(AstNode *node, int code, const char *msg, ...);
void errorMsgLex(int code, const char *msg, ...);
void errorSummary();

#endif