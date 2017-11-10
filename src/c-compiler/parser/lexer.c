/** Lexer
 * @file
 *
 * The lexer divides up the source program into tokens, producing each for the parser on demand.
 * The lexer assumes UTF-8 encoding for the source program.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "lexer.h"
#include "../shared/ast.h"
#include "../shared/memory.h"
#include "../shared/error.h"

#include <string.h>
#include <stdlib.h>

// Global lexer state
Lexer *lex = NULL;		// Current lexer

// Inject a new source stream into the lexer
void lexInject(char *url, char *src) {
	Lexer *prev;

	// Obtain next lexer block via link chain or allocation
	prev = lex;
	if (lex == NULL)
		lex = (Lexer*) memAllocBlk(sizeof(Lexer));
	else if (lex->next == NULL) {
		lex->next = (Lexer*) memAllocBlk(sizeof(Lexer));
		lex = lex->next;
	}
	else
		lex = lex->next; // Re-use an old lexer block
	lex->next = NULL;
	lex->prev = prev;

	// Initialize lexer's source info
	lex->url = url;
	lex->source = src;

	// Initialize lexer context
	lex->srcp = lex->tokp = lex->linep = src;
	lex->linenbr = 1;
	lex->flags = 0;

	// Prime the pump with the first token
	lexNextToken();
}

// Restore previous lexer's stream
void lexPop() {
	if (lex)
		lex = lex->prev;
}

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
		lex->token->asttype = FloatNode;
	}
	else {
		lex->token->v.uintlit = intval;
	}
	lex->srcp = srcp;
}

// Initialize an AstNode for a token
#define lex_node_init(asttyp) { \
	AstNode *tok = lex->token; \
	tok->asttype = asttyp; \
	tok->lexer = lex; \
	tok->srcp = srcp; \
	tok->linep = lex->linep; \
	tok->linenbr = lex->linenbr; \
}

// Decode next token from the source into new lex->token
void lexNextToken() {
	char *srcp;
	AstNode *token;
	token = lex->token = (struct AstNode*) memAllocBlk(sizeof(AstNode));
	token->flags = 0;
	srcp = lex->srcp;
	while (1) {
		switch (*srcp) {

		// End-of-file
		case '\0': case '\x1a':
			lex->token->asttype = EofNode;
			return;

		// Ignore white space
		case ' ': case '\t': case '\r':
			srcp++;
			break;

		// Handle new line
		case '\n':
			srcp++;
			lex->linep = srcp;
			lex->linenbr++;
			break;

		// Numeric literal (integer or float)
		case '0': case '1': case '2': case '3': case '4': 
		case '5': case '6': case '7': case '8': case '9':
			lex_node_init(IntNode);	// May change to FloatNode
			lexScanNumber(srcp);
			return;

		// '-'
		case '-':
			lex_node_init(MinusNode);
			lex->srcp = ++srcp;
			return;

		// Bad character
		default:
			lex_node_init(0);
			errorMsg(lex->token, ErrorBadTok, "Bad character '%c' starting unknown token", *srcp);
			srcp++;
		}
	}
}

// Return current node's ast type
uint16_t lexGetType() {
	return lex->token->asttype;
}

// Return current token's node after figuring out the next one
AstNode *lexGetAndNext() {
	AstNode *node = lex->token;
	lexNextToken();
	return node;
}

// Return current node's token if it matches the specified ast type
AstNode *lexMatch(uint16_t nodetype) {
	if (lex->token->asttype == nodetype) {
		AstNode *node = lex->token;
		lexNextToken();
		return node;
	}
	else
		return NULL;
}
