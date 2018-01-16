/** AST handling for expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef expr_h
#define expr_h

// Cast to another type
typedef struct CastAstNode {
	TypedAstHdr;
	AstNode *exp;
} CastAstNode;

typedef struct DerefAstNode {
	TypedAstHdr;
	AstNode *exp;
} DerefAstNode;

// Logic operator: not, or, and
typedef struct LogicAstNode {
	TypedAstHdr;
	AstNode *lexp;
	AstNode *rexp;
} LogicAstNode;

CastAstNode *newCastAstNode(AstNode *exp, AstNode *type);
void castPrint(CastAstNode *node);
void castPass(AstPass *pstate, CastAstNode *node);

DerefAstNode *newDerefAstNode();
void derefPrint(DerefAstNode *node);
void derefPass(AstPass *pstate, DerefAstNode *node);

LogicAstNode *newLogicAstNode(int16_t typ);
void logicPrint(LogicAstNode *node);
void logicPass(AstPass *pstate, LogicAstNode *node);
void logicNotPass(AstPass *pstate, LogicAstNode *node);

#endif