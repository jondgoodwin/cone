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
} VarAstNode;

// Unary operator (e.g., negative)
typedef struct UnaryAstNode {
	TypedAstHdr;
	AstNode *expnode;
} UnaryAstNode;

ULitAstNode *newULitNode(uint64_t nbr, AstNode *type);
void ulitPrint(int indent, ULitAstNode *node);

FLitAstNode *newFLitNode(double nbr, AstNode *type);
void flitPrint(int indent, FLitAstNode *node);

#endif