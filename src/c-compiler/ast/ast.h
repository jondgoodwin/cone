/** AST encoding definitions: AstNode, AstType
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef ast_h
#define ast_h

#include "../shared/memory.h"
#include "../ast/nodes.h"
typedef struct Symbol Symbol;	// ../shared/symbol.h
typedef struct Lexer Lexer;		// ../parser/lexer.h

#include <stdint.h>

// AST Groupings
enum AstGroup {
	VoidGroup,	// Statements that return no value
	ExpGroup,	// Nodes that output a value
	VTypeGroup,	// Nodes that describe a value type
	PermGroup,	// Nodes that describe a permission type
	AllocGroup	// Nodes that describe an allocator type
};

// Retrieve the group for an ast type
#define astgroup(typ) ((typ)>>8)

// All the possible types for an AstNode
enum AstType {
	// Lexer-only nodes that are *never* found in a program's AST.
	// KeywordNode exists for symbol table consistency
	KeywordNode = (VoidGroup<<8),	// Keyword token (flags is the keyword's token type)

	// Untyped (Basic) AST nodes
	PgmNode,		// Program (global area)
	BlockNode,		// Block (list of statements)

	// Expression nodes (having value type)
	ULitNode = (ExpGroup<<8),		// Integer literal
	FLitNode,		// Float literal
	VarNode,		// Variable node
	UnaryNode,		// Unary method operator
	FnImplNode,		// Function implementation (vtype is its signature)

	// AST nodes that are value types
	VoidType = (VTypeGroup<<8),	// representing no values, e.g., no return values on a fn

	// NbrTypeAstNode
	IntType,	// Integer
	UintType,	// Unsigned integer
	FloatType,	// Floating point number

	PtrType,	// Also smart pointers?

	ArrayType,	// Also dynamic arrays? SOA?

	FnSig,		// Also method, closure, behavior, co-routine, thread, ...

	StructType,	// Also class, interface, actor, etc.?

	EnumType,	// Also sum type, etc.?

	ModuleType,	// Modules, Generics ?

	// Permission type nodes
	PermType = (PermGroup<<8),

	// Allocator type nodes
	AllocType = (AllocGroup<<8),
};

// All AST nodes begin with this header, mostly containing lexer data describing
// where in the source this structure came from (useful for error messages)
// - asttype contains the AstType code
// - flags contains node-specific flags
// - lexer contains -> url (filepath) and -> source
// - srcp points to start of source token
// - linep points to start of line that token begins on
// - linenbr is the source file's line number, starting with 1
#define BasicAstHdr \
	Lexer *lexer; \
	char *srcp; \
	char *linep; \
	uint32_t linenbr; \
	uint16_t asttype; \
	uint16_t flags

// AstNode is a castable struct for all AST nodes.
typedef struct AstNode {
	BasicAstHdr;
} AstNode;

// Typed Ast Node header, offering access to the node's type info
// - vtype is the value type for an expression (e.g., 'i32')
#define TypedAstHdr \
	BasicAstHdr; \
	AstNode *vtype

// Castable structure for all typed AST nodes
typedef struct TypedAstNode {
	TypedAstHdr;
} TypedAstNode;

// Named Ast Node header, for all nodes having a symbol table entry
// - name points to the symbol table entry, where char* and hash can be found
// - prev points to the AstNode for the node held in earlier scope for this name
// - lifetime starts with 0=global, increments for inner local lexical blocks
#define NamedAstHdr \
	TypedAstHdr; \
	Symbol *name; \
	AstNode *prev; \
	uint16_t lifetime; \
	uint16_t moreinfo

// Castable structure for all named AST nodes
typedef struct NamedAstNode {
	NamedAstHdr;
} NamedAstNode;

#include "../ast/block.h"
#include "../ast/expr.h"
#include "../types/type.h"
#include "../types/fnsig.h"
#include "../types/number.h"
#include "../types/permission.h"

// Allocate and initialize a new AST node
#define newAstNode(node, aststruct, asttyp) {\
	node = (aststruct*) memAllocBlk(sizeof(aststruct)); \
	node->asttype = asttyp; \
	node->flags = 0; \
	node->lexer = lex; \
	node->srcp = lex->tokp; \
	node->linep = lex->linep; \
	node->linenbr = lex->linenbr; \
}

void astPrint(AstNode *pgm);
void astPrintNode(int indent, AstNode *node, char *prefix);
void astPrintLn(int indent, char *str, ...);

#endif