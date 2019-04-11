/** Handling for allocate expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef allocate_h
#define allocate_h

typedef struct AllocateNode {
    ITypedNodeHdr;
    INode *exp;
} AllocateNode;

AllocateNode *newAllocateNode();
void allocatePrint(AllocateNode *node);

// Name resolution of allocate node
void allocateNameRes(PassState *pstate, AllocateNode **nodep);

// Type check allocate node
void allocatePass(PassState *pstate, AllocateNode **node);

// Perform data flow analysis on addr node
void allocateFlow(FlowState *fstate, AllocateNode **nodep);

#endif