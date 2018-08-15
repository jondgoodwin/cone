/** Handling for expression nodes that might do copy/move
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fncall_h
#define fncall_h

// Function or method call node
// The contents varies after parse vs. after type check due to lowering.
typedef struct FnCallNode {
	ITypedNodeHdr;
	INode *objfn;        // Object (for method calls) or function to call
    NameUseNode *methprop;    // Name of method/property (or NULL)
	Nodes *args;           // List of function call arguments (or NULL)
} FnCallNode;

FnCallNode *newFnCallNode(INode *objfn, int nnodes);
FnCallNode *newFnCallOp(INode *obj, char *op, int nnodes);
void fnCallPrint(FnCallNode *node);
void fnCallPass(PassState *pstate, FnCallNode *node);

#endif