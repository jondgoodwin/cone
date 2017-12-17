/** AST handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef literal_h
#define literal_h

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

ULitAstNode *newULitNode(uint64_t nbr, AstNode *type);
void ulitPrint(ULitAstNode *node);

FLitAstNode *newFLitNode(double nbr, AstNode *type);
void flitPrint(FLitAstNode *node);

int litIsLiteral(AstNode* node);

#endif