/** Lexer
 * @file
 *
 * The lexer divides up the source program into tokens, producing each for the parser on demand.
 * The lexer assumes UTF-8 encoding for the source program.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"

#include <string.h>
#include <stdlib.h>

// Lexer state per source file scanned and parsed
typedef struct Lexer {
	AstNode *token;	// Current token
	char *url;		// The url where the source text came from

	// Program source pointers
	char *source;	// The source text
	char *srcp;		// Current pointer
	char *tokp;		// Start of current token
	char *linep;	// Pointer to start of current line

	uint32_t linenbr;	// Current line number
	uint32_t flags;		// Lexer flags
} Lexer;

// Global lexer state
Lexer *lex = NULL;		// Current lexer

/** Tokenize an integer or floating point number */
void lexScanNumber(char *srcp) {

	char *srcbeg;		// Pointer to the start of the token
	uint64_t base;		// Radix for integer (10 or 16)
	uint64_t intval;	// Calculated integer value for integer literal
	char isFloat;		// nonzero when number token is a float, 'e' when in exponent

	srcbeg = srcp;

	// A leading zero may indicate a non-base 10 number
	base = 10;
	if (*srcp=='0' && (*(srcp+1)=='x' || *(srcp+1)=='X')) {
		base = 16;
		srcp += 2;
	}

	// Validate and process remaining numeric digits
	isFloat = '\0';
	intval = 0;
	while (1) {
		// Just scan for valid characters in floating point number
		if (isFloat) {
			// Only one exponent allowed
			if (isFloat!='e' && (*srcp=='e' || *srcp=='E')) {
				isFloat = 'e';
				if (*++srcp=='-')
					srcp++;
				continue;
			}
			if ((*srcp<'0' || *srcp>'9') && *srcp!='_')
				break;
			srcp++;
		}
		// Handle characters in a suspected integer
		else {
			// Decimal point means it is floating point after all
			if (*srcp=='.') {
				// However, double periods is not floating point, but that subsequent token is range op
				if (*(srcp+1)=='.')
					break;
				srcp++;
				isFloat = '.';
				continue;
			}
			// Extract a number digit value from the character
			if (*srcp>='0' && *srcp<='9')
				intval = intval*base + *srcp++ - '0';
			else if (*srcp=='_')
				srcp++;
			else if (base==16) {
				if (*srcp>='A' && *srcp<='F')
					intval = (intval<<4) + *srcp++ - 'A'+10;
				else if (*srcp>='a' && *srcp<='f')
					intval = (intval<<4) + *srcp++ - 'a'+10;
				else
					break;
			}
			else
				break;
		}
	}

	// Process number's explicit type as part of the token

	// Set value and type
	if (isFloat) {
		lex->token->v.floatlit = atof(srcbeg);
		lex->token->type = LitNode;
	}
	else {
		lex->token->v.uintlit = intval;
		lex->token->type = LitNode;
	}
	lex->srcp = srcp;
}

// Retrieve next token from the lexer
void lexGetNextToken() {
	char *srcp;
	AstNode *token;
	token = lex->token = (struct AstNode*) memAllocBlk(sizeof(AstNode));
	token->flags = 0;
	srcp = lex->srcp;
	while (1) {
		switch (*srcp) {

		// End-of-file
		case '\0': case '\x1a':
			lex->token->type = EofNode;
			return;

		// Ignore white space
		case ' ': case '\t':
			srcp++;
			break;

		// Numeric literal (integer or float)
		case '0': case '1': case '2': case '3': case '4': 
		case '5': case '6': case '7': case '8': case '9':
			lexScanNumber(srcp);
			return;

		// Bad character
		default:
			srcp++;
		}
	}
}

uint16_t lexType() {
	return lex->token->type;
}

AstNode *lexMatch(uint16_t nodetype) {
	if (lex->token->type == nodetype) {
		AstNode *node = lex->token;
		lexGetNextToken();
		return node;
	}
	else
		return NULL;
}

// Create a new lexer for program source file
Lexer *lexNew(char *url) {
	// Allocate lexer block and load source
	lex = (Lexer*) memAllocBlk(sizeof(Lexer));
	lex->url = url;
	if (!(lex->source = fileLoad(url))) {
		// error message
		lex->source = "";
	}
	// Initialize lexer block
	lex->srcp = lex->tokp = lex->linep = lex->source;
	lex->linenbr = 1;
	lex->flags = 0;
	lex->token = NULL;
	lexGetNextToken(); // Prime the pump
	return lex;
}