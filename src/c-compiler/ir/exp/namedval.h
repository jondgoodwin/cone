/** Handling for named value nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef namedval_h
#define namedval_h

// Cast to another type
typedef struct NamedValNode {
    ITypedNodeHdr;
    INode *name;
    INode *val;
} NamedValNode;

NamedValNode *newNamedValNode(INode *name);
void namedValPrint(NamedValNode *node);
void namedValWalk(PassState *pstate, NamedValNode *node);

#endif