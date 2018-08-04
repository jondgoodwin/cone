/** AST handling for expression nodes: Literals, Variables, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/memory.h"
#include "../../parser/lexer.h"
#include "../nametbl.h"

// Create a new function signature node
FnSigAstNode *newFnSigNode() {
	FnSigAstNode *sig;
	newAstNode(sig, FnSigAstNode, FnSigType);
	sig->parms = newNodes(8);
	sig->rettype = voidType;
	return sig;
}

// Serialize the AST for a function signature
void fnSigPrint(FnSigAstNode *sig) {
	AstNode **nodesp;
	uint32_t cnt;
	astFprint("fn(");
	for (nodesFor(sig->parms, cnt, nodesp)) {
		astPrintNode(*nodesp);
		if (cnt > 1)
			astFprint(", ");
	}
	astFprint(") ");
	astPrintNode(sig->rettype);
}

// Traverse the function signature tree
void fnSigPass(PassState *pstate, FnSigAstNode *sig) {
	AstNode **nodesp;
	uint32_t cnt;
	for (nodesFor(sig->parms, cnt, nodesp))
		nodeWalk(pstate, nodesp);
	nodeWalk(pstate, &sig->rettype);
}

// Compare two function signatures to see if they are equivalent
int fnSigEqual(FnSigAstNode *node1, FnSigAstNode *node2) {
	AstNode **nodes1p, **nodes2p;
	int16_t cnt;

	// Return types and number of parameters must match
	if (!typeIsSame(node1->rettype, node2->rettype)
		|| node1->parms->used != node2->parms->used)
		return 0;

	// Every parameter's type must also match
	nodes2p = &nodesGet(node2->parms, 0);
	for (nodesFor(node1->parms, cnt, nodes1p)) {
		if (!typeIsSame(*nodes1p, *nodes2p))
			return 0;
		nodes2p++;
	}
	return 1;
}

// Will the function call (caller) be able to call the 'to' function
// Return 0 if not. 1 if perfect match. 2+ for imperfect matches
int fnSigMatchesCall(FnSigAstNode *to, Nodes *args) {
	int matchsum = 1;

	// Too many arguments is not a match
	if (args->used > to->parms->used)
		return 0;

	// Every parameter's type must also match
	AstNode **tonodesp;
	AstNode **callnodesp;
	int16_t cnt;
	tonodesp = &nodesGet(to->parms, 0);
	for (nodesFor(args, cnt, callnodesp)) {
		int match;
		switch (match = typeMatches(((TypedAstNode *)*tonodesp)->vtype, ((TypedAstNode*)*callnodesp)->vtype)) {
		case 0: return 0;
		case 1: break;
		default: matchsum += match;
		}
		tonodesp++;
	}
	// Match fails if not enough arguments & method has no default values on parms
	if (args->used != to->parms->used 
		&& ((VarDclAstNode *)tonodesp)->value==NULL)
		return 0;

	// It is a match; return how perfect a match it is
	return matchsum;
}