/** Lexer
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef lexer_h
#define lexer_h

typedef struct AstNode AstNode;	// ../ast/ast.h
typedef struct Name Name;	// ../ast/nametbl.h

#include <stdint.h>

// Lexer state (one per source file)
typedef struct Lexer {
	// Value info about a discovered token
	union {
		double floatlit;
		uint64_t uintlit;
		char *strlit;
		Name *ident;
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

	// if nbrcurly > 0, offside rule is turned off
	int16_t nbrcurly;	// Number of explicit curly braces active
	// if nbrtoks > 1, implicit semicolon needed to end statement
	int16_t nbrtoks;	// Number of tokens (+1) in current stmt
	int16_t curindent;	// Indentation level of current line
	char indentch;		// Are we using spaces or tabs?
} Lexer;

// All the possible types for a token
enum TokenTypes {
	EofToken,		// End-of-file

	// Numeric and Identifier tokens
	IntLitToken,	// Integer literal
	FloatLitToken,	// Float literal
	StrLitToken,	// String literal
	IdentToken,		// Identifier
	PermToken,		// Permission identifier

	// Punctuation tokens
	SemiToken,			// ';'
	ColonToken,			// ':'
	DblColonToken,		// '::'
	LCurlyToken,		// '{'
	RCurlyToken,		// '}'
	LBracketToken,		// '['
	RBracketToken,		// ']'
	LParenToken,		// '('
	RParenToken,		// ')'
	CommaToken,			// ','
	DotToken,			// '.'
	PlusToken,			// '+'
	DashToken,			// '-'
	StarToken,			// '*'
	PercentToken,		// '%'
	SlashToken,			// '/'
	AmperToken,			// '&'
	AndToken,			// '&&'
	BarToken,			// '|'
	OrToken,			// '||'
	CaretToken,			// '^'
	NotToken,			// '!'
	TildeToken,			// '~'
	UnderscoreToken,	// '_'
	AssgnToken,			// '='
	EqToken,			// '=='
	NeToken,			// '!='
	LtToken,			// '<'
	LeToken,			// '<='
	GtToken,			// '>'
	GeToken,			// '>='

	// Keywords
	IncludeToken,	// 'include'
	ModToken,		// 'mod'
	ExternToken,	// 'extern'
	FnToken,		// 'fn'
	StructToken,	// 'struct'
	AllocToken,		// 'alloc'
	RetToken,		// 'return'
	IfToken,		// 'if'
	ElifToken,		// 'elif'
	ElseToken,		// 'else'
	WhileToken,		// 'while'
	BreakToken,		// 'break'
	ContinueToken,	// 'continue'
	AsToken,		// 'as'
	trueToken,		// 'true'
	falseToken,		// 'false'

	NbrTokens
};

// Current lexer
Lexer *lex;

#define lexIsToken(tok) (lex->toktype == (tok))

// Lexer functions
void lexInjectFile(char *url);
void lexInject(char *url, char *src);
void lexPop();
void lexNextToken();

#endif
