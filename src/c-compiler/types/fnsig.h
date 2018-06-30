/** AST handling for function signature
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fnsig_h
#define fnsig_h

typedef struct FnCallAstNode FnCallAstNode;

// Function signature is a type that defines the parameters and return type for a function.
// A function signature is never named (although a ptr/ref to a fnsig may be named).
// The parameter declaration list represents a local namespace of local variables.
typedef struct FnSigAstNode {
	TypeAstHdr;
    Inodes *parms;			// Indexed collection mapping parm names to nodes
    AstNode *rettype;		// void, a single type or a type tuple
} FnSigAstNode;

FnSigAstNode *newFnSigNode();
void fnSigPrint(FnSigAstNode *node);
void fnSigPass(PassState *pstate, FnSigAstNode *name);
int fnSigEqual(FnSigAstNode *node1, FnSigAstNode *node2);
int fnSigMatchesCall(FnSigAstNode *to, FnCallAstNode *from);

#endif