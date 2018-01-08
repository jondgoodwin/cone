/** AST structures and macros
*
* The AST, together with the symbol table, is the skeleton of the compiler.
* It connects together every stage:
*
* - The parser transforms programs into AST
* - The semantic analysis walks the AST over multiple passes
* - Macro and template expansion happens via AST cloning
* - Generation transforms AST into LLVM IR
*
* The AST is comprised of heterogeneous nodes that share common BasicAstHdr info.
* In some cases, it is possible for several distinct node types to share an 
* identical data structure (e.g., statement expression and return).
*
* This include file will pull in the include files for all node types, including types.
* It also defines the structures needed for semantic analysis passes.
*
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

#include <llvm-c/Core.h>

#include <stdint.h>

typedef struct AstPass AstPass;

// AST groupings - every node belongs to one of these groups
enum AstGroup {
	VoidGroup,	// Statements that return no value
	ExpGroup,	// Nodes that return a value
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
	OpCodeNode,		// Alternative to fndcl block for internal operations (e.g., add)
	ReturnNode,		// Return node
	WhileNode,		// While node
	BreakNode,		// Break node
	ContinueNode,	// Continue node

	// Name usage (we do not know what type of name it is until name resolution pass)
	NameUseNode,	// Name use node

	// Expression nodes (having value type - or sometimes nullType)
	VarNameDclNode = (ExpGroup<<8),
	VarNameUseNode,	// Variable reference
	FieldNameUseNode,	// Member of a namespace
	ULitNode,		// Integer literal
	FLitNode,		// Float literal
	AssignNode,		// Assignment expression
	FnCallNode,		// Function call
	CastNode,		// Cast exp to another type
	AddrNode,		// & (address of) operator
	DerefNode,		// * (pointed at) operator
	NotLogicNode,	// ! / not
	OrLogicNode,	// || / or
	AndLogicNode,	// && / and
	BlockNode,		// Block (list of statements)
	IfNode,			// if .. elif .. else statement

	// Value type AST nodes
	VtypeNameDclNode = (VTypeGroup<<8),
	VtypeNameUseNode,
	VoidType,	// representing no values, e.g., no return values on a fn
	IntNbrType,	// Integer
	UintNbrType,	// Unsigned integer
	FloatNbrType,	// Floating point number
	PtrType,	// Also smart pointers?
	ArrayType,	// Also dynamic arrays? SOA?
	FnSig,		// Also method, closure, behavior, co-routine, thread, ...
	StructType,	// Also class, interface, actor, etc.?
	EnumType,	// Also sum type, etc.?
	ModuleType,	// Modules, Generics ?

	// Permission type nodes
	PermNameDclNode = (PermGroup << 8),
	PermNameUseNode,
	PermType,

	// Allocator type nodes
	AllocNameDclNode = (AllocGroup<<8),
	AllocNameUseNode,
	AllocType,
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

// Type Ast Node header for all type structures
// - instnames is the list of an instance's named methods and properties/fields
// - typenames is the list of the type's named functions and constants
// - traits is the list of all named traits the type implements
#define TypeAstHdr \
	TypedAstHdr; \
	Inodes *instnames; \
	Inodes *typenames; \
	Nodes *traits

// Castable structure for all type AST nodes
typedef struct TypeAstNode {
	TypeAstHdr;
} TypeAstNode;


#include "../types/permission.h"
#include "../ast/block.h"
#include "../ast/expr.h"
#include "../ast/name.h"
#include "../ast/literal.h"
#include "../types/type.h"
#include "../types/fnsig.h"
#include "../types/number.h"
#include "../types/pointer.h"

// The AST analytical passes performed in between parse and generation
enum Passes {
	// Scope all declared names and resolve all name uses accordingly
	NameResolution,
	// Do return inference and type inference/checks.
	TypeCheck
};

// Context used across all AST semantic analysis passes
typedef struct AstPass {
	int pass;				// Passes
	FnSigAstNode *fnsig;	// The type signature of the function we are within
	BlockAstNode *blk;		// The current block we are within

	int16_t scope;			// The current block scope (0=global, 1=fnsig, 2+=blocks)
	uint16_t flags;
} AstPass;

#define PassWithinWhile 0x0001

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

void astPrint(char *dir, char *srcfn, AstNode *pgm);
void astPrintNode(AstNode *node);
void astFprint(char *str, ...);
void astPrintNL();
void astPrintIndent();
void astPrintIncr();
void astPrintDecr();

void astPasses(PgmAstNode *pgm);
void astPass(AstPass *pstate, AstNode *pgm);

#endif