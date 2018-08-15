/** AST handling for expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef expr_h
#define expr_h

// Get size of a type
typedef struct SizeofNode {
	ITypedNodeHdr;
	INode *type;
} SizeofNode;

// Cast to another type
typedef struct CastNode {
	ITypedNodeHdr;
	INode *exp;
} CastNode;

typedef struct DerefNode {
	ITypedNodeHdr;
	INode *exp;
} DerefNode;

// Logic operator: not, or, and
typedef struct LogicNode {
	ITypedNodeHdr;
	INode *lexp;
	INode *rexp;
} LogicNode;

SizeofNode *newSizeofNode();
void sizeofPrint(SizeofNode *node);
void sizeofPass(PassState *pstate, SizeofNode *node);

CastNode *newCastNode(INode *exp, INode *type);
void castPrint(CastNode *node);
void castPass(PassState *pstate, CastNode *node);

DerefNode *newDerefNode();
void derefPrint(DerefNode *node);
void derefPass(PassState *pstate, DerefNode *node);
void derefAuto(INode **node);

LogicNode *newLogicNode(int16_t typ);
void logicPrint(LogicNode *node);
void logicPass(PassState *pstate, LogicNode *node);
void logicNotPass(PassState *pstate, LogicNode *node);

#endif