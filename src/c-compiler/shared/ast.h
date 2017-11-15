/** AST encoding definitions: AstNode, AstType
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef ast_h
#define ast_h

#include "type.h"

#include <stdint.h>

typedef struct Lexer Lexer;

// All AST nodes begin with this header, mostly containing lexer data describing
// where in the source this structure came from (useful for error messages)
// - lexer contains -> url (filepath) and -> source
// - srcp points to start of source token
// - linep points to start of line that token begins on
// - linenbr is the source file's line number, starting with 1
#define AstNodeHdr \
	Lexer *lexer; \
	char *srcp; \
	char *linep; \
	uint32_t linenbr; \
	uint16_t asttype; \
	uint16_t subtype

// AstNode encodes every type of token or AST node in a fixed-size structure.
// A single structure converges both purposes, as most tokens are proto-AST nodes.
typedef struct AstNode {
	AstNodeHdr;
} AstNode;

// Expression Ast Node header, offering consistent access into to the value's type
#define ExpAstNodeHdr \
	AstNodeHdr; \
	LangTypeInfo *vtype

// Unsigned integer literal
typedef struct ULitAstNode {
	ExpAstNodeHdr;
	uint64_t uintlit;
} ULitAstNode;

// Float literal
typedef struct FLitAstNode {
	ExpAstNodeHdr;
	double floatlit;
} FLitAstNode;

// Unary operator (e.g., negative)
typedef struct UnaryAstNode {
	ExpAstNodeHdr;
	AstNode *expnode;
} UnaryAstNode;

// Header for a variable-sized structure holding a list of AstNodes
// The nodes immediately follow the header
typedef struct Nodes {
	uint32_t used;
	uint32_t avail;
} Nodes;

// Block
typedef struct BlockAstNode {
	AstNodeHdr;
	Nodes *nodes;
} BlockAstNode;

// All the possible types for an AstNode
enum AstType {
	UnkNode,		// Unknown token (bad character)

	BlockNode,		// Block (list of statements)

	ULitNode,		// Integer literal
	FLitNode,		// Float literal

	UnaryNode,		// Unary method operator

	NbrAstTypes
};

// Allocate and initialize a new AST node, then retrieve next token
#define astNewNodeAndNext(node, aststruct, asttyp) {\
	astNewNode(node, aststruct, asttyp); \
	lexNextToken(); \
}

// Allocate and initialize a new AST node
#define astNewNode(node, aststruct, asttyp) {\
	node = (aststruct*) memAllocBlk(sizeof(aststruct)); \
	node->asttype = asttyp; \
	node->lexer = lex; \
	node->srcp = lex->srcp; \
	node->linep = lex->linep; \
	node->linenbr = lex->linenbr; \
}

// Helper Functions
Nodes *nodesNew(int size);
void nodesAdd(Nodes **nodesp, AstNode *node);

#endif