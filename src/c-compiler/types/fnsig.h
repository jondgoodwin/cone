/** AST handling for function signature
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fnsig_h
#define fnsig_h

// For function signatures
typedef struct FnSigAstNode {
	TypedAstHdr;
	AstNode *rettype;		// void, one or tuple
	// Inodes *parms;		// void or
} FnSigAstNode;

FnSigAstNode *newFnSigNode();
void fnSigPrint(FnSigAstNode *node);
void fnSigPass(AstPass *pstate, FnSigAstNode *name);
int fnSigEqual(FnSigAstNode *node1, FnSigAstNode *node2);

#endif