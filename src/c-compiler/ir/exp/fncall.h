/** Handling for expression nodes that might do copy/move
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fncall_h
#define fncall_h

// Function or method call node. Also used for array indexing and field access.
// The parsed contents is lowered during type checking, potentially turning
// it into an ArrIndexTag or StrFieldTag node
typedef struct FnCallNode {
    IExpNodeHdr;
    INode *objfn;          // Object (for method calls) or function to call
    NameUseNode *methfld;  // Name of method/field (or NULL)
    Nodes *args;           // List of function call arguments (or NULL)
} FnCallNode;

FnCallNode *newFnCallNode(INode *objfn, int nnodes);
// Create new fncall node, prefilling method, self, and creating room for nnodes args
FnCallNode *newFnCallOpname(INode *obj, Name *opname, int nnodes);
FnCallNode *newFnCallOp(INode *obj, char *op, int nnodes);
void fnCallPrint(FnCallNode *node);

// Name resolution on 'fncall'
// - If node is indexing on a type, retag node as a typelit
// Note: this never name resolves .methfld, which is handled in type checking
void fnCallNameRes(NameResState *pstate, FnCallNode **nodep);

// Type check on fncall
void fnCallTypeCheck(TypeCheckState *pstate, FnCallNode **node);

// Do data flow analysis for fncall node (only real function calls)
void fnCallFlow(FlowState *fstate, FnCallNode **nodep);

#endif