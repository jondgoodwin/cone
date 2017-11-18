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
// - asttype contains the AstType code
// - flags contains node-specific flags
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
	uint16_t flags

// AstNode is a castable struct for all AST nodes.
typedef struct AstNode {
	AstNodeHdr;
} AstNode;

// Identifier that refers to a type
typedef struct TypeAstNode {
	AstNodeHdr;
	LangTypeInfo *type;
	char *name;
} TypeAstNode;

// Expression Ast Node header, offering access to the node's type info
// - vtype is the value type for an expression (e.g., 'i32')
// - perm is the permission type (e.g., 'mut')
// - alloc is the allocator type (e.g., 'global')
#define ExpAstNodeHdr \
	AstNodeHdr; \
	LangTypeInfo *vtype; \
	LangTypeInfo *perm; \
	LangTypeInfo *alloc

// ExpAstNode is a castable struct for all expression nodes,
// providing convenient access to the expression's type info
typedef struct ExpAstNode {
	ExpAstNodeHdr;
} ExpAstNode;

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

// Variable identifier
typedef struct VarAstNode {
	ExpAstNodeHdr;
	char *name;
} VarAstNode;

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

// Program
typedef struct PgmAstNode {
	AstNodeHdr;
	Nodes *nodes;
} PgmAstNode;

// Function block
typedef struct FnBlkAstNode {
	AstNodeHdr;
	Nodes *nodes;
} FnBlkAstNode;

// Block
typedef struct BlockAstNode {
	AstNodeHdr;
	Nodes *nodes;
} BlockAstNode;

// All the possible types for an AstNode
enum AstType {
	// Type node
	TypeNode,		// Identifier that refers to a type

	// Expression nodes (having value, permission, alloc types)
	ULitNode,		// Integer literal
	FLitNode,		// Float literal
	VarNode,		// Variable node
	UnaryNode,		// Unary method operator

	PgmNode,		// Program (global area)
	FnBlkNode,		// Function block
	BlockNode,		// Block (list of statements)

	// Lexer-only nodes that are *never* found in a program's AST.
	// KeywordNode exists for symbol table consistency
	// UnkNode exists so the lexer can generate an error message
	KeywordNode,	// Keyword token (flags is the keyword's token type)
	UnkNode,		// Unknown 'bad' token

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