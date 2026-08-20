/** Handling for swap nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef swap_h
#define swap_h

// Swap node
typedef struct {
    INodeHdr;
    INode *lval;
    INode *rval;
} SwapNode;

SwapNode *newSwapNode(INode *lval, INode *rval);

// Clone assign
INode *cloneSwapNode(CloneState *cstate, SwapNode *node);

void swapPrint(SwapNode *node);

// Name resolution for assignment node
void swapNameRes(NameResState *pstate, SwapNode *node);

// Type check for assignment node
void swapTypeCheck(TypeCheckState *pstate, SwapNode *node);

// Perform data flow analysis on assignment node
void swapFlow(FlowState *fstate, SwapNode **node);

#endif
