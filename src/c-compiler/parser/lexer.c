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
#include "../shared/type.h"
#include "../shared/symbol.h"
#include "../shared/globals.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../shared/utf8.h"

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

	// Skip over UTF8 Byte-order mark (BOM = U+FEFF) at start of source, if there
	if (*src=='\xEF' && *(src+1)=='\xBB' && *(src+2)=='\xBF')
		src += 3;

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
	if (*srcp=='d') {
		srcp++;
		lex->langtype = f64Type;
	} else if (*srcp=='f') {
		lex->langtype = f32Type;
		if (*(++srcp)=='6' && *(srcp+1)=='4') {
			lex->langtype = f64Type;
			srcp += 2;
		}
		else if (*srcp=='3' && *(srcp+1)=='2')
			srcp += 2;
	} else if (*srcp=='i') {
		lex->langtype = i32Type;
		if (*(++srcp)=='8') {		
			srcp++; lex->langtype = i8Type;
		} else if (*srcp=='1' && *(srcp+1)=='6') {
			srcp += 2; lex->langtype = i16Type;
		} else if (*srcp=='3' && *(srcp+1)=='2') {
			srcp += 2;
		} else if (*srcp=='6' && *(srcp+1)=='4') {
			srcp += 2; lex->langtype = i64Type;
		} else if (strncmp(srcp, "size", 4)==0) {
			srcp += 2; lex->langtype = target.ptrsize==64? i64Type : i32Type;
		}
	} else if (*srcp=='u') {
		lex->langtype = u32Type;
		if (*(++srcp)=='8') {		
			srcp++; lex->langtype = u8Type;
		} else if (*srcp=='1' && *(srcp+1)=='6') {
			srcp += 2; lex->langtype = u16Type;
		} else if (*srcp=='3' && *(srcp+1)=='2') {
			srcp += 2;
		} else if (*srcp=='6' && *(srcp+1)=='4') {
			srcp += 2; lex->langtype = u64Type;
		} else if (strncmp(srcp, "size", 4)==0) {
			srcp += 2; lex->langtype = target.ptrsize==64? u64Type : u32Type;
		}
	}

	// Set value and type
	if (isFloat) {
		lex->val.floatlit = atof(srcbeg);
		lex->toktype = FloatLitToken;
	}
	else {
		lex->val.uintlit = intval;
		lex->toktype = IntLitToken;
	}
	lex->srcp = srcp;
}

/** Tokenize an identifier or reserved token */
void lexScanIdent(char *srcp) {
	char *srcbeg = srcp++;	// Pointer to the start of the token
	while (1) {
		switch (*srcp) {

		// Allow digit, letter or underscore in token
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
		case 'a': case 'b': case 'c': case 'd': case 'e':
		case 'f': case 'g': case 'h': case 'i': case 'j':
		case 'k': case 'l': case 'm': case 'n': case 'o':
		case 'p': case 'q': case 'r': case 's': case 't':
		case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
		case 'A': case 'B': case 'C': case 'D': case 'E':
		case 'F': case 'G': case 'H': case 'I': case 'J':
		case 'K': case 'L': case 'M': case 'N': case 'O':
		case 'P': case 'Q': case 'R': case 'S': case 'T':
		case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
		case '_':
			srcp++;
			break;

		default:
			// Allow unicode letters in identifier name
			if (utf8IsLetter(srcp))
				srcp += utf8ByteSkip(srcp);
			else {
				AstNode *identNode;
				// Identifier may end with '?'
				if (*srcp == '?')
					srcp++;
				// Find identifier token in symbol table and preserve info about it
				// Substitute token type when identifier is a keyword
				lex->val.ident = symFind(srcbeg, srcp-srcbeg);
				identNode = lex->val.ident->node;
				lex->toktype = (identNode && identNode->asttype == KeywordNode)? identNode->flags : IdentToken;
				lex->srcp = srcp;
				return;
			}
		}
	}
}

// Decode next token from the source into new lex->token
void lexNextToken() {
	char *srcp;
	srcp = lex->srcp;
	while (1) {
		switch (*srcp) {

		// Numeric literal (integer or float)
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			lexScanNumber(srcp);
			return;

		// Identifier
		case 'a': case 'b': case 'c': case 'd': case 'e':
		case 'f': case 'g': case 'h': case 'i': case 'j':
		case 'k': case 'l': case 'm': case 'n': case 'o':
		case 'p': case 'q': case 'r': case 's': case 't':
		case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
		case 'A': case 'B': case 'C': case 'D': case 'E':
		case 'F': case 'G': case 'H': case 'I': case 'J':
		case 'K': case 'L': case 'M': case 'N': case 'O':
		case 'P': case 'Q': case 'R': case 'S': case 'T':
		case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
		case '$':
			lexScanIdent(srcp);
			return;

		// '_' or identifier that starts with underscore
		case '_':
			if (utf8IsLetter(srcp+1))
				lexScanNumber(srcp);
			else {
				lex->toktype = UnderscoreToken;
				lex->srcp = ++srcp;
			}
			return;

		// '-'
		case '-':
			lex->toktype = DashToken;
			lex->srcp = ++srcp;
			return;

		// '/' or '//' or '/*'
		case '/':
			// Line comment: '//'
			if (*(srcp+1)=='/') {
				srcp += 2;
				while (*srcp && *srcp!='\n' && *srcp!='\x1a')
					srcp++;
			}
			// '/' operator (e.g., division)
			else {
				lex->toktype = SlashToken;
				lex->srcp = ++srcp;
				return;
			}

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

		// End-of-file
		case '\0': case '\x1a':
			lex->toktype = EofToken;
			return;

		// Bad character
		default:
			{
				AstNode *node;
				lex->srcp = srcp;
				astNewNode(node, AstNode, UnkNode);
				errorMsg(node, ErrorBadTok, "Bad character '%c' starting unknown token", *srcp);
				srcp += utf8ByteSkip(srcp);
			}
		}
	}
}
