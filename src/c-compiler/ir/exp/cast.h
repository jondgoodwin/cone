/** Handling for cast nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef cast_h
#define cast_h

// Cast to another type
typedef struct CastNode {
	ITypedNodeHdr;
	INode *exp;
} CastNode;

CastNode *newCastNode(INode *exp, INode *type);
void castPrint(CastNode *node);
void castPass(PassState *pstate, CastNode *node);

#endif