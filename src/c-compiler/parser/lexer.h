/** Lexer
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef lexer_h
#define lexer_h

#include "../shared/type.h"

#include <stdint.h>

typedef struct AstNode AstNode;

// Lexer state (one per source file)
typedef struct Lexer {
	// Value info about a discovered token
	union {
		double floatlit;
		uint64_t uintlit;
	} val;
	LangTypeInfo *langtype;

	// immutable info about source
	char *url;		// The url where the source text came from
	char *source;	// The source text (0-terminated)

	struct Lexer *next;	// Next lexer (linked list of injected lexers)
	struct Lexer *prev; // Previous lexer

	// Lexer's evolving state
	char *srcp;		// Current pointer
	char *tokp;		// Start of current token
	char *linep;	// Pointer to start of current line

	uint32_t linenbr;	// Current line number
	uint32_t flags;		// Lexer flags
	uint16_t toktype;	// TokenTypes
} Lexer;

// All the possible types for a token
enum TokenTypes {
	EofToken,		// End-of-file

	IntLitToken,	// Integer literal
	FloatLitToken,	// Float literal

	DashOpToken,	// '-'

	NbrTokens
};

// Current lexer
Lexer *lex;

// Lexer functions
void lexInject(char *url, char *src);
void lexPop();
void lexNextToken();

#endif
