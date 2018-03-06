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
#include "keyword.h"
#include "../ast/ast.h"
#include "../shared/nametbl.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../shared/utf8.h"
#include "../shared/fileio.h"

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
	lex->fname = fileName(url);
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

/** Return value of hex digit, or -1 if not correct */
char *lexHexDigits(int cnt, char *srcp, uint64_t *val) {
	*val = 0;
	while (cnt--) {
		*val <<= 4;
		if (*srcp>='0' && *srcp<='9')
			*val += *srcp++ - '0';
		else if (*srcp>='A' && *srcp<='F')
			*val += *srcp++ - ('A' - 10);
		else if (*srcp>='a' && *srcp<='f')
			*val += *srcp++ - ('a' - 10);
		else {
			errorMsgLex(ErrorBadTok, "Invalid hexadecimal character '%c'", *srcp);
			return srcp;
		}
	}
	return srcp;
}

/** Turn escape sequence into a single character */
char *lexScanEscape(char *srcp, uint64_t *charval) {
	switch (*++srcp) {
	case 'a': *charval = '\a'; return ++srcp;
	case 'b': *charval = '\b'; return ++srcp;
	case 'f': *charval = '\f'; return ++srcp;
	case 'n': *charval = '\n'; return ++srcp;
	case 'r': *charval = '\r'; return ++srcp;
	case 't': *charval = '\t'; return ++srcp;
	case 'v': *charval = '\v'; return ++srcp;
	case '\'': *charval = '\''; return ++srcp;
	case '\"': *charval = '\"'; return ++srcp;
	case '\\': *charval = '\\'; return ++srcp;
	case '\0': *charval = '\0'; return ++srcp;
	case 'x': return lexHexDigits(2, ++srcp, charval);
	case 'u': return lexHexDigits(4, ++srcp, charval);
	case 'U': return lexHexDigits(8, ++srcp, charval);
	default:
		errorMsgLex(ErrorBadTok, "Invalid escape sequence '%c'", *srcp);
		*charval = *srcp++;
		return srcp;
	}
}

/** Tokenize a character */
void lexScanChar(char *srcp) {
	lex->tokp = srcp++;
	if (*srcp == '\\')
		srcp = lexScanEscape(srcp, &lex->val.uintlit);
	else
		lex->val.uintlit = *srcp++;
	if (*srcp == '\'')
		srcp++;
	else
		errorMsgLex(ErrorBadTok, "Only one character allowed in character literal");
	lex->langtype = (AstNode*)u32Type;
	lex->toktype = IntLitToken;
	lex->srcp = srcp;
}

void lexScanString(char *srcp) {
	uint64_t uchar;
	lex->tokp = srcp++;

	// Conservatively count the size of the string
	uint32_t srclen = 0;
	while (*srcp != '"') {
		srclen++;
		srcp++;
	}

	// Build string literal
	char *newp = memAllocStr(NULL, srclen);
	lex->val.strlit = newp;
	srcp = lex->tokp+1;
	while (*srcp != '"') {
		if (*srcp == '\\')
			srcp = lexScanEscape(srcp, &uchar);
		else
			uchar = *srcp++;
		if (uchar<0x80)
			*newp++ = (unsigned char)uchar;
		else if (uchar<0x800) {
			*newp++ = 0xC0 | (unsigned char)(uchar >> 6);
			*newp++ = 0x80 | (uchar & 0x3f);
		}
		else if (uchar<0x10000) {
			*newp++ = 0xE0 | (unsigned char)(uchar >> 12);
			*newp++ = 0x80 | ((uchar >> 6) & 0x3F);
			*newp++ = 0x80 | (uchar & 0x3f);
		}
		else if (uchar<0x110000) {
			*newp++ = 0xF0 | (unsigned char)(uchar >> 18);
			*newp++ = 0x80 | ((uchar >> 12) & 0x3F);
			*newp++ = 0x80 | ((uchar >> 6) & 0x3F);
			*newp++ = 0x80 | (uchar & 0x3f);
		}
	}
	*newp = '\0';
	srcp++;

	lex->langtype = (AstNode*)strType;
	lex->toktype = StrLitToken;
	lex->srcp = srcp;
}

/** Tokenize an integer or floating point number */
void lexScanNumber(char *srcp) {

	char *srcbeg;		// Pointer to the start of the token
	uint64_t base;		// Radix for integer (10 or 16)
	uint64_t intval;	// Calculated integer value for integer literal
	char isFloat;		// nonzero when number token is a float, 'e' when in exponent

	lex->tokp = srcbeg = srcp;

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
		isFloat = 'd';
		srcp++;
		lex->langtype = (AstNode*)f64Type;
	} else if (*srcp=='f') {
		isFloat = 'f';
		lex->langtype = (AstNode*)f32Type;
		if (*(++srcp)=='6' && *(srcp+1)=='4') {
			lex->langtype = (AstNode*)f64Type;
			srcp += 2;
		}
		else if (*srcp=='3' && *(srcp+1)=='2')
			srcp += 2;
	} else if (*srcp=='i') {
		lex->langtype = (AstNode*)i32Type;
		if (*(++srcp)=='8') {		
			srcp++; lex->langtype = (AstNode*)i8Type;
		} else if (*srcp=='1' && *(srcp+1)=='6') {
			srcp += 2; lex->langtype = (AstNode*)i16Type;
		} else if (*srcp=='3' && *(srcp+1)=='2') {
			srcp += 2;
		} else if (*srcp=='6' && *(srcp+1)=='4') {
			srcp += 2; lex->langtype = (AstNode*)i64Type;
		} else if (strncmp(srcp, "size", 4)==0) {
			srcp += 4; lex->langtype = (AstNode*)isizeType;
		}
	} else if (*srcp=='u') {
		lex->langtype = (AstNode*)u32Type;
		if (*(++srcp)=='8') {		
			srcp++; lex->langtype = (AstNode*)u8Type;
		} else if (*srcp=='1' && *(srcp+1)=='6') {
			srcp += 2; lex->langtype = (AstNode*)u16Type;
		} else if (*srcp=='3' && *(srcp+1)=='2') {
			srcp += 2;
		} else if (*srcp=='6' && *(srcp+1)=='4') {
			srcp += 2; lex->langtype = (AstNode*)u64Type;
		} else if (strncmp(srcp, "size", 4)==0) {
			srcp += 4; lex->langtype = (AstNode*)usizeType;
		}
	}
	else
		lex->langtype = (AstNode*)(isFloat ? f32Type : i32Type);

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
	lex->tokp = srcbeg;
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
				// Find identifier token in name table and preserve info about it
				// Substitute token type when identifier is a keyword
				lex->val.ident = nameFind(srcbeg, srcp-srcbeg);
				identNode = (AstNode*)lex->val.ident->node;
				lex->toktype = (identNode && identNode->asttype == KeywordNode)? identNode->flags : IdentToken;
				lex->srcp = srcp;
				return;
			}
		}
	}
}

/** Tokenize an identifier or reserved token */
void lexScanTickedIdent(char *srcp) {
	char *srcbeg = srcp++;	// Pointer to the start of the token
	lex->tokp = srcbeg;

	// Look for closing backtick, but not past end of line
	while (*srcp != '`' && *srcp && *srcp != '\n' && *srcp != '\x1a')
		srcp++;
	if (*srcp != '`') {
		errorMsgLex(ErrorBadTok, "Back-ticked identifier requires closing backtick");
		srcp = srcbeg + 2;
	}

	// Find identifier token in name table and preserve info about it
	lex->val.ident = nameFind(srcbeg+1, srcp - srcbeg - 1);
	lex->toktype = IdentToken;
	lex->srcp = srcp+1;
}

// Skip over nested block comment
char *lexBlockComment(char *srcp) {
	int nest = 1;
	while (*srcp) {
		if (*srcp == '*' && *(srcp + 1) == '/') {
			if (--nest == 0)
				return srcp+2;
			++srcp;
		}
		else if (*srcp == '/' && *(srcp + 1) == '*') {
			++nest;
			++srcp;
		}
		// ignore tokens inside line comment
		else if (*srcp == '/' && *(srcp + 1) == '/') {
			srcp += 2;
			while (*srcp && *srcp++ != '\n');
		}
		// ignore tokens inside string literal
		else if (*srcp == '"') {
			++srcp;
			while (*srcp && *srcp++ != '"') {
				if (*(srcp - 1) == '\\' && *srcp == '"')
					srcp++;
			}
		}
		++srcp;
	}
	return srcp;
}

// Shortcut macro for return a punctuation token
#define lexReturnPuncTok(tok, skip) { \
	lex->toktype = tok; \
	lex->tokp = srcp; \
	lex->srcp = srcp + skip; \
	return; \
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

		// ' ' - single character surrounded with single quotes
		case '\'':
			lexScanChar(srcp);
			return;

		// " " - string surrounded with double quotes
		case '"':
			lexScanString(srcp);
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
			if (utf8IsLetter(srcp+1) || *(srcp + 1) == '_' || *(srcp + 1) == '$' || (*(srcp+1)>='0' && *(srcp+1)<='9'))
				lexScanIdent(srcp);
			else {
				lex->toktype = UnderscoreToken;
				lex->srcp = ++srcp;
			}
			return;

		// backtick enclosed identifiers
		case '`':
			lexScanTickedIdent(srcp);
			return;

		case '.': lexReturnPuncTok(DotToken, 1);
		case ',': lexReturnPuncTok(CommaToken, 1);
		case '-': lexReturnPuncTok(DashToken, 1);
		case '*': lexReturnPuncTok(StarToken, 1);
		case '+': lexReturnPuncTok(PlusToken, 1);
		case '%': lexReturnPuncTok(PercentToken, 1);
		case '~': lexReturnPuncTok(TildeToken, 1);
		case '^': lexReturnPuncTok(CaretToken, 1);

		case '&': 
			if (*(srcp + 1) == '&') {
				lexReturnPuncTok(AndToken, 2);
			}
			else {
				lexReturnPuncTok(AmperToken, 1);
			}

		case '|': 
			if (*(srcp + 1) == '|') {
				lexReturnPuncTok(OrToken, 2);
			}
			else {
				lexReturnPuncTok(BarToken, 1);
			}

		// '=' and '=='
		case '=':
			if (*(srcp + 1) == '=')	{
				lexReturnPuncTok(EqToken, 2);
			}
			else {
				lexReturnPuncTok(AssgnToken, 1);
			}

		// '!' and '!='
		case '!':
			if (*(srcp + 1) == '=') {
				lexReturnPuncTok(NeToken, 2);
			}
			else {
				lexReturnPuncTok(NotToken, 1);
			}

		// '<' and '<='
		case '<':
			if (*(srcp + 1) == '=') {
				lexReturnPuncTok(LeToken, 2);
			}
			else {
				lexReturnPuncTok(LtToken, 1);
			}

		// '>' and '>='
		case '>':
			if (*(srcp + 1) == '=') {
				lexReturnPuncTok(GeToken, 2);
			}
			else {
				lexReturnPuncTok(GtToken, 1);
			}

		case '{': lexReturnPuncTok(LCurlyToken, 1);
		case '}': lexReturnPuncTok(RCurlyToken, 1);
		case '[': lexReturnPuncTok(LBracketToken, 1);
		case ']': lexReturnPuncTok(RBracketToken, 1);
		case '(': lexReturnPuncTok(LParenToken, 1);
		case ')': lexReturnPuncTok(RParenToken, 1);

		// ';'
		case ';':
			lexReturnPuncTok(SemiToken, 1);

		// '/' or '//' or '/*'
		case '/':
			// Line comment: '//'
			if (*(srcp+1)=='/') {
				srcp += 2;
				while (*srcp && *srcp!='\n' && *srcp!='\x1a')
					srcp++;
			}
			// Block comment, nested: '/*'
			else if (*(srcp + 1) == '*') {
				srcp = lexBlockComment(srcp+2);
			}
			// '/' operator (e.g., division)
			else
				lexReturnPuncTok(SlashToken, 1);
			break;

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
			lexReturnPuncTok(EofToken, 0);

		// Bad character
		default:
			{
				lex->tokp = srcp;
				errorMsgLex(ErrorBadTok, "Bad character '%c' starting unknown token", *srcp);
				srcp += utf8ByteSkip(srcp);
			}
		}
	}
}
