/** AST handling for expression nodes: Literals, Variables, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"

// Create a new function signature node
FnSigAstNode *newFnSigNode() {
	FnSigAstNode *sig;
	newAstNode(sig, FnSigAstNode, FnSig);
	sig->instnames = newInodes(1); // probably share these across all fnsigs
	sig->typenames = newInodes(1); // ditto
	sig->traits = newNodes(1);    // ditto
	sig->parms = newInodes(8);
	sig->rettype = voidType;
	sig->vtype = NULL;
	return sig;
}

// Serialize the AST for a function signature
void fnSigPrint(FnSigAstNode *sig) {
	SymNode *nodesp;
	uint32_t cnt;
	astFprint("fn(");
	for (inodesFor(sig->parms, cnt, nodesp)) {
		astPrintNode(nodesp->node);
		if (cnt > 1)
			astFprint(", ");
	}
	astFprint(") ");
	astPrintNode(sig->rettype);
}

// Traverse the function signature tree
void fnSigPass(AstPass *pstate, FnSigAstNode *sig) {
	SymNode *nodesp;
	uint32_t cnt;
	for (inodesFor(sig->parms, cnt, nodesp))
		astPass(pstate, nodesp->node);
	astPass(pstate, sig->rettype);
}

// Compare two function signatures to see if they are equivalent
int fnSigEqual(FnSigAstNode *node1, FnSigAstNode *node2) {
	return node1->rettype == node2->rettype;
}