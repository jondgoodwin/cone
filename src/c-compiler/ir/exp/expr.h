/** AST handling for expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef expr_h
#define expr_h

// Get size of a type
typedef struct SizeofAstNode {
	TypedAstHdr;
	INode *type;
} SizeofAstNode;

// Cast to another type
typedef struct CastAstNode {
	TypedAstHdr;
	INode *exp;
} CastAstNode;

typedef struct DerefAstNode {
	TypedAstHdr;
	INode *exp;
} DerefAstNode;

// Logic operator: not, or, and
typedef struct LogicAstNode {
	TypedAstHdr;
	INode *lexp;
	INode *rexp;
} LogicAstNode;

SizeofAstNode *newSizeofAstNode();
void sizeofPrint(SizeofAstNode *node);
void sizeofPass(PassState *pstate, SizeofAstNode *node);

CastAstNode *newCastAstNode(INode *exp, INode *type);
void castPrint(CastAstNode *node);
void castPass(PassState *pstate, CastAstNode *node);

DerefAstNode *newDerefAstNode();
void derefPrint(DerefAstNode *node);
void derefPass(PassState *pstate, DerefAstNode *node);
void derefAuto(INode **node);

LogicAstNode *newLogicAstNode(int16_t typ);
void logicPrint(LogicAstNode *node);
void logicPass(PassState *pstate, LogicAstNode *node);
void logicNotPass(PassState *pstate, LogicAstNode *node);

#endif