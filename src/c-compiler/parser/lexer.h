/** Lexer
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef lexer_h
#define lexer_h

typedef struct AstNode AstNode;	// ../ast/ast.h
typedef struct Symbol Symbol;	// ../shared/symbol.h

#include <stdint.h>

// Lexer state (one per source file)
typedef struct Lexer {
	// Value info about a discovered token
	union {
		double floatlit;
		uint64_t uintlit;
		Symbol *ident;
	} val;
	AstNode *langtype;

	// immutable info about source
	char *url;		// The url where the source text came from
	char *fname;	// The filename of the url (no extension)
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

	// Numeric and Identifier tokens
	IntLitToken,	// Integer literal
	FloatLitToken,	// Float literal
	IdentToken,		// Identifier

	// Punctuation tokens
	SemiToken,			// ';'
	LCurlyToken,		// '{'
	RCurlyToken,		// '}'
	LParenToken,		// '('
	RParenToken,		// ')'
	DashToken,			// '-'
	SlashToken,			// '/'
	UnderscoreToken,	// '_'

	// Keywords
	FnToken,		// 'fn'
	RetToken,		// 'return'

	// Number types
	i8Token,
	i16Token,
	i32Token,
	i64Token,
	u8Token,
	u16Token,
	u32Token,
	u64Token,
	f32Token,
	f64Token,

	// Permission types
	mutToken,
	mmutToken,
	immToken,
	constToken,
	constxToken,
	mutxToken,
	idToken,

	NbrTokens
};

// Current lexer
Lexer *lex;

#define lexIsToken(tok) (lex->toktype == (tok))

// Lexer functions
void lexInject(char *url, char *src);
void lexPop();
void lexNextToken();

#endif
