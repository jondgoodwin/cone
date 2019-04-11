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

#define FlagAsIf 0x8000

CastNode *newCastNode(INode *exp, INode *type);
void castPrint(CastNode *node);

// Name resolution of cast node
void castNameRes(NameResState *pstate, CastNode *node);

// Type check cast node:
// - reinterpret cast types must be same size
// - Ensure type can be safely converted to target type
void castTypeCheck(TypeCheckState *pstate, CastNode *node);

#endif