/** Handling for allocate expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef allocate_h
#define allocate_h

// Uses RefNode from reference.h

void allocatePrint(RefNode *node);

// Name resolution for questag: decide if Option type or fold into AllocNode
void allocateQuesNameRes(NameResState *pstate, FnCallNode **nodep);

// Type check allocate node
void allocateTypeCheck(TypeCheckState *pstate, RefNode **node);

// Perform data flow analysis on addr node
void allocateFlow(FlowState *fstate, RefNode **nodep);

#endif
