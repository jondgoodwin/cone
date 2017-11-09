/** Lexer
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef lexer_h
#define lexer_h

#include <stdint.h>

typedef struct AstNode AstNode;

// Lexer state (one per source file)
typedef struct Lexer {
	// immutable info about source
	char *url;		// The url where the source text came from
	char *source;	// The source text (0-terminated)

	struct Lexer *next;	// Next lexer (linked list of injected lexers)
	struct Lexer *prev; // Previous lexer

	// Lexer's evolving state
	AstNode *token;	// Current token
	char *srcp;		// Current pointer
	char *tokp;		// Start of current token
	char *linep;	// Pointer to start of current line

	uint32_t linenbr;	// Current line number
	uint32_t flags;		// Lexer flags
} Lexer;

void lexInject(char *url, char *src);
void lexPop();
void lexGetNextToken();
uint16_t lexType();
AstNode *lexMatch(uint16_t nodetype);

#endif
