/** AST handling for expression nodes: Literals, Variables, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef expr_h
#define expr_h

// Unsigned integer literal
typedef struct ULitAstNode {
	TypedAstHdr;
	uint64_t uintlit;
} ULitAstNode;

// Float literal
typedef struct FLitAstNode {
	TypedAstHdr;
	double floatlit;
} FLitAstNode;

// Variable identifier
typedef struct VarAstNode {
	NamedAstHdr;
	AstNode *perm;	// Permission (often mut or imm)
	AstNode *value;	// May be NULL if not initialized
} VarAstNode;

// Unary operator (e.g., negative)
typedef struct UnaryAstNode {
	TypedAstHdr;
	AstNode *expnode;
} UnaryAstNode;

VarAstNode *newVarNode(Symbol *name, AstNode *sig, AstNode *perm);
void varPrint(int indent, VarAstNode *fn);
void varPass(AstPass *pstate, VarAstNode *node);

ULitAstNode *newULitNode(uint64_t nbr, AstNode *type);
void ulitPrint(int indent, ULitAstNode *node);

FLitAstNode *newFLitNode(double nbr, AstNode *type);
void flitPrint(int indent, FLitAstNode *node);

#endif