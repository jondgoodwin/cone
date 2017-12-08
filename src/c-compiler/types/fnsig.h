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
	Symbol *namesym;
	AstNode *rettype;		// void, one or tuple
	// Inodes *parms;		// void or
} FnSigAstNode;

FnSigAstNode *newFnSigNode();
void fnsigPrint(int indent, FnSigAstNode *node, char *prefix);
int fnSigEqual(FnSigAstNode *node1, FnSigAstNode *node2);

#endif