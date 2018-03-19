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
	ExitSuccess,
	ExitError,	// Program fails to compile due to caught errors
	ExitNF,		// Could not find specified source files
	ExitMem,	// Out of memory
	ExitOpts,	// Invalid compiler options

	// Non-terminating errors
	ErrorCode = 1000,
	ErrorBadTok,	// Bad character starting an unknown token
	ErrorGenErr,	// Failure to perform generation activity
	ErrorNoSemi,	// Missing semicolon
	ErrorNoLCurly,  // Missing left curly brace
	ErrorNoRCurly,	// Missing right curly brace
	ErrorBadTerm,   // Invalid term - something other than var, lit, etc.
	ErrorBadGloStmt, // Invalid global area type, var or function statement
	ErrorDupImpl,	// Function already has another implementation
	ErrorNoLParen,  // Expected left parenthesis not found
	ErrorNoRParen,	// Expected right parenthesis not found
	ErrorDupName,	// Duplicate name declaration
	ErrorNoName,	// Name is required but not provided
	ErrorInvType,	// Types do not match correctly
	ErrorNoIdent,	// Identifier expected but not provided
	ErrorNotLit,	// Value can only be a literal
	ErrorBadLval,	// Expression is not an lval
	ErrorNoMut,		// Mutation is not allowed
	ErrorNotFn,		// Not a function
	ErrorUnkName,	// Unknown name (no declaration exists)
	ErrorNoType,	// No type specified (or inferrable)
	ErrorNoInit,	// Parm didn't specify required default value
	ErrorFewArgs,	// Too few arguments specified
	ErrorManyArgs,	// Too many arguments specified
	ErrorNoMbr,		// Field/method not specified
	ErrorNoMeth,	// No such method defined by the object's type
	ErrorRetNotLast, // Return was found not at the end of the block
	ErrorNoRet,		// Return value expected but not given
	ErrorNoElse,	// Missing 'else' branch
	ErrorNoWhile,	// 'break' or 'continue' allowed only in while/each loop
	ErrorNoVtype,	// Missing value type
	ErrorNotPtr,	// Not a pointer
	ErrorNotLval,	// Not an lval
	ErrorAddr,		// Invalid expr to get an addr (&) of
	ErrorBadPerm,	// Permission not allowed
	ErrorNoFlds,	// Expression's type does not support fields
	ErrorBadAlloc,  // Missing valid alloc methods
	ErrorNoDbl,		// Missing '::'
	ErrorNoVar,		// Missing variable name
	ErrorNoEof,		// Missing end-of-file

	// Warnings
	WarnCode = 3000,
	WarnName,		// Unnecessary name
};

int errors;

// Send an error message to stderr
void errorExit(int exitcode, const char *msg, ...);
void errorMsgNode(AstNode *node, int code, const char *msg, ...);
void errorMsgLex(int code, const char *msg, ...);
void errorMsg(int code, const char *msg, ...);
void errorSummary();

#endif