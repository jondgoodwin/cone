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
	AstNode *type;
} SizeofAstNode;

// Cast to another type
typedef struct CastAstNode {
	TypedAstHdr;
	AstNode *exp;
} CastAstNode;

typedef struct DerefAstNode {
	TypedAstHdr;
	AstNode *exp;
} DerefAstNode;

typedef struct ElementAstNode {
	TypedAstHdr;
	AstNode *owner;
	AstNode *element;
} ElementAstNode;

// Logic operator: not, or, and
typedef struct LogicAstNode {
	TypedAstHdr;
	AstNode *lexp;
	AstNode *rexp;
} LogicAstNode;

SizeofAstNode *newSizeofAstNode();
void sizeofPrint(SizeofAstNode *node);
void sizeofPass(PassState *pstate, SizeofAstNode *node);

CastAstNode *newCastAstNode(AstNode *exp, AstNode *type);
void castPrint(CastAstNode *node);
void castPass(PassState *pstate, CastAstNode *node);

DerefAstNode *newDerefAstNode();
void derefPrint(DerefAstNode *node);
void derefPass(PassState *pstate, DerefAstNode *node);
void derefAuto(AstNode **node);

ElementAstNode *newElementAstNode();
void elementPrint(ElementAstNode *node);
void elementPass(PassState *pstate, ElementAstNode *node);

LogicAstNode *newLogicAstNode(int16_t typ);
void logicPrint(LogicAstNode *node);
void logicPass(PassState *pstate, LogicAstNode *node);
void logicNotPass(PassState *pstate, LogicAstNode *node);

#endif