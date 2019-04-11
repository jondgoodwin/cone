/** Handling for sizeof nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef sizeof_h
#define sizeof_h

// Get size of a type
typedef struct SizeofNode {
    ITypedNodeHdr;
    INode *type;
} SizeofNode;

SizeofNode *newSizeofNode();
void sizeofPrint(SizeofNode *node);

// Name resolution of sizeof node
void sizeofNameRes(NameResState *pstate, SizeofNode *node);

// Type check sizeof node
void sizeofTypeCheck(TypeCheckState *pstate, SizeofNode *node);

#endif