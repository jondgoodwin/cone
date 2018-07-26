/** AST handling for expression nodes that might do copy/move
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fncall_h
#define fncall_h

// Function or method call node
// The contents varies after parse vs. after type check due to lowering.
typedef struct FnCallAstNode {
	TypedAstHdr;
	AstNode *objfn;        // Object (for method calls) or function to call
    NameUseAstNode *methfield;    // Name of method/field (or NULL)
	Nodes *args;           // List of function call arguments (or NULL)
} FnCallAstNode;

FnCallAstNode *newFnCallAstNode(AstNode *objfn, int nnodes);
FnCallAstNode *newFnCallOp(AstNode *obj, char *op, int nnodes);
void fnCallPrint(FnCallAstNode *node);
void fnCallPass(PassState *pstate, FnCallAstNode *node);

#endif