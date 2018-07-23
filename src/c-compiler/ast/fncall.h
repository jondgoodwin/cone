/** AST handling for expression nodes that might do copy/move
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fncall_h
#define fncall_h

// Function call node
typedef struct FnCallAstNode {
	TypedAstHdr;
	AstNode *objfn;
    AstNode *method;
	Nodes *args;
} FnCallAstNode;

FnCallAstNode *newFnCallAstNode(AstNode *objfn, int nnodes);
void fnCallPrint(FnCallAstNode *node);
void fnCallPass(PassState *pstate, FnCallAstNode *node);

#endif