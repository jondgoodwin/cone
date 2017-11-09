/** AST encoding definitions: AstNode, AstType
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef ast_h
#define ast_h

#include <stdint.h>

typedef struct Lexer Lexer;

// AstNode encodes every type of token or AST node in a fixed-size structure.
// A single structure converges both purposes, as most tokens are proto-AST nodes.
typedef struct AstNode {
	// The "value" information held by the node
	union {
		double floatlit;
		uint64_t uintlit;
		void *val;
	} v;
	struct AstNode *typeinfo;

	// Token/Node's location in the source program
	Lexer *lexer;	// Contains -> url (filepath) and -> source
	char *srcp;		// Points to start of source token
	char *linep;	// Points to start of line that token begins on
	uint32_t linenbr;	// Line number (starting with 1)

	// Type information about the Token/AST Node
	short int type;			// AstType
	uint16_t flags;
} AstNode;

// All the possible types for an AstNode
enum AstType {
	EofNode,		// End-of-file

	IntNode,		// Integer literal
	FloatNode,		// Float literal

	NbrAstTypes
};

#endif