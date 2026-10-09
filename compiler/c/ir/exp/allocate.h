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

// Type check an allocation written with the retired '+', which is refused
void allocateTypeCheck(TypeCheckState *pstate, RefNode **node);

// Type check an allocation whose value is already checked
void allocateValueCheck(TypeCheckState *pstate, RefNode **nodep);

// Set while an owner of a callable is made (typeLitNewCallable): a callable that
// holds nothing is a value with no size, and its allocation is the smallest block
// the region gives, freed as any is
extern int allocateZeroSizeOk;

// Perform data flow analysis on addr node
void allocateFlow(FlowState *fstate, RefNode **nodep);

#endif
