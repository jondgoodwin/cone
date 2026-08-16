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
// it into an ArrIndexTag or FldAccessTag node
typedef struct FnCallNode {
    IExpNodeHdr;
    INode *objfn;          // Object (for method calls) or function to call
    INode *methfld;        // Method or field node after '.' operator (typically MbrNameUseNode or NULL)
    Nodes *args;           // List of function call arguments (or NULL)
} FnCallNode;

FnCallNode *newFnCallNode(INode *objfn, int nnodes);
// Create new fncall node, prefilling method, self, and creating room for nnodes args
FnCallNode *newFnCallOpname(INode *obj, Name *opname, int nnodes);
FnCallNode *newFnCallOp(INode *obj, char *op, int nnodes);
// The '...Lower' forms position the new node on an existing one rather than on
// wherever the lexer has reached, which is what a node synthesized after its
// construct was parsed needs.
FnCallNode *newFnCallOpnameLower(INode *oldnode, INode *obj, Name *opname, int nnodes);
FnCallNode *newFnCallLower(INode *oldnode, INode *obj, int nnodes);

// Clone fncall
INode *cloneFnCallNode(CloneState *cstate, FnCallNode *node);

void fnCallPrint(FnCallNode *node);

// Name resolution on 'fncall'
// - If node is indexing on a type, retag node as a typelit
// Note: this never name resolves .methfld, which is handled in type checking
void fnCallNameRes(NameResState *pstate, FnCallNode **nodep);

// Type check on fncall
void fnCallTypeCheck(TypeCheckState *pstate, FnCallNode **node);

// Find the one field or method that accepts the call's receiver and arguments,
// then lower the node to a function call (objfn+args) or field access (objfn+methfld).
// A receiver held through a reference or pointer is dereferenced where the selected
// method declared 'self' by value; nothing is borrowed on the receiver's behalf.
// Returns 1 when lowered, 0 when the receiver's type supports no methods at all
// (so the caller may try another way), and -1 when a diagnostic was reported.
int fnCallLowerMethod(FnCallNode *callnode);

// We have a reference or pointer, and a method to find (comparison or arithmetic)
// If found, lower the node to a function call (objfn+args)
// Otherwise try again against the type it points to
int fnCallLowerPtrMethod(FnCallNode *callnode, INsTypeNode *methtype);

// objfn names an overload set. Select the one candidate that accepts the call's
// arguments, rewrite the call to that concrete function, then finalize its arguments.
void fnCallLowerOverloadFn(FnCallNode *node);

// Do data flow analysis for fncall node (only real function calls)
void fnCallFlow(FlowState *fstate, FnCallNode **nodep);

// Perform data flow analysis on array index node
void fnCallArrIndexFlow(FlowState *fstate, FnCallNode **node);

// Perform data flow analysis on field access node
void fnCallFldAccessFlow(FlowState *fstate, FnCallNode **node);

#endif
