/** AST handling for expression nodes that might do value copies/moves
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"
#include "../shared/error.h"

#include <assert.h>

void handleCopy(AstPass *pstate, AstNode *node) {

}

// Create a new assignment node
AssignAstNode *newAssignAstNode(int16_t assigntype, AstNode *lval, AstNode *rval) {
	AssignAstNode *node;
	newAstNode(node, AssignAstNode, AssignNode);
	node->assignType = assigntype;
	node->lval = lval;
	node->rval = rval;
	return node;
}

// Serialize assignment node
void assignPrint(AssignAstNode *node) {
	astFprint("(=, ");
	astPrintNode(node->lval);
	astFprint(", ");
	astPrintNode(node->rval);
	astFprint(")");
}

// expression is valid lval expression
int isLval(AstNode *node) {
	switch (node->asttype) {
	case NameUseNode:
	case DerefNode:
	case ElementNode:
		return 1;
	// future:  [] indexing and .member
	default: break;
	}

	return 0;
}

// Analyze assignment node
void assignPass(AstPass *pstate, AssignAstNode *node) {
	astPass(pstate, node->lval);
	astPass(pstate, node->rval);

	switch (pstate->pass) {
	case TypeCheck:
		if (!isLval(node->lval))
			errorMsgNode(node->lval, ErrorBadLval, "Expression to left of assignment must be lval");
		else if (!typeCoerces(node->lval, &node->rval))
			errorMsgNode(node->rval, ErrorInvType, "Expression's type does not match lval's type");
		else if (!permIsMutable(node->lval))
			errorMsgNode(node->lval, ErrorNoMut, "You do not have permission to modify lval");
		else
			handleCopy(pstate, node->rval);
		node->vtype = ((TypedAstNode*)node->rval)->vtype;
	}
}

// Create a function call node
FnCallAstNode *newFnCallAstNode(AstNode *fn, int nnodes) {
	FnCallAstNode *node;
	newAstNode(node, FnCallAstNode, FnCallNode);
	node->fn = fn;
	node->parms = newNodes(nnodes);
	return node;
}

// Serialize function call node
void fnCallPrint(FnCallAstNode *node) {
	AstNode **nodesp;
	uint32_t cnt;
	astPrintNode(node->fn);
	astFprint("(");
	for (nodesFor(node->parms, cnt, nodesp)) {
		astPrintNode(*nodesp);
		if (cnt > 1)
			astFprint(", ");
	}
	astFprint(")");
}

NameDclAstNode *fnCallFindMethod(FnCallAstNode *node, Symbol *methsym) {
	// Get type of object call's object (first arg). Use value type of a ref
	AstNode *objtype = typeGetVtype(*nodesNodes(node->parms));
	if (objtype->asttype == RefType || objtype->asttype == PtrType)
		objtype = typeGetVtype(((PtrAstNode *)objtype)->pvtype);

	// Look for best-fit method among those defined for the type
	int bestnbr = 0x7fffffff; // ridiculously high number
	NameDclAstNode *bestmethod = NULL;
	AstNode **nodesp;
	uint32_t cnt;
	for (nodesFor(((TypeAstNode*)objtype)->methods, cnt, nodesp)) {
		NameDclAstNode *method = (NameDclAstNode*)*nodesp;
		if (method->namesym == methsym) {
			int match;
			switch (match = fnSigMatchesCall((FnSigAstNode *)method->vtype, node)) {
			case 0: continue;		// not an acceptable match
			case 1: return method;	// perfect match!
			default:				// imprecise match using conversions
				if (match < bestnbr) {
					// Remember this as best found so far
					bestnbr = match;
					bestmethod = method;
				}
			}
		}
	}
	return bestmethod;
}

// Analyze function call node
void fnCallPass(AstPass *pstate, FnCallAstNode *node) {
	AstNode **argsp;
	uint32_t cnt;
	for (nodesFor(node->parms, cnt, argsp))
astPass(pstate, *argsp);
astPass(pstate, node->fn);

switch (pstate->pass) {
case TypeCheck:
{
	// If this is an object call, resolve method name within first argument's type
	if (node->fn->asttype == MemberUseNode) {
		NameUseAstNode *methname = (NameUseAstNode*)node->fn;
		Symbol *methsym = methname->namesym;
		NameDclAstNode *method;
		if (method = fnCallFindMethod(node, methsym)) {
			methname->asttype = NameUseNode;
			methname->dclnode = method;
			methname->vtype = methname->dclnode->vtype;
		}
		else {
			errorMsgNode((AstNode*)node, ErrorNoMeth, "The method `%s` is not defined by the object's type.", &methsym->namestr);
			return;
		}
	}

	// Automatically deref a reference to the function
	else
		derefAuto(&node->fn);

	// Capture return vtype and ensure we are calling a function
	AstNode *fnsig = typeGetVtype(node->fn);
	if (fnsig->asttype == FnSig)
		node->vtype = ((FnSigAstNode*)fnsig)->rettype;
	else {
		errorMsgNode(node->fn, ErrorNotFn, "Cannot call a value that is not a function");
		return;
	}

	// Error out if we have too many arguments
	int argsunder = ((FnSigAstNode*)fnsig)->parms->used - node->parms->used;
	if (argsunder < 0) {
		errorMsgNode((AstNode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
		return;
	}

	// Type check that passed arguments match declared parameters
	AstNode **argsp;
	uint32_t cnt;
	SymNode *parmp = (SymNode*)((((FnSigAstNode*)fnsig)->parms) + 1);
	for (nodesFor(node->parms, cnt, argsp)) {
		if (!typeCoerces(parmp->node, argsp))
			errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
		else
			handleCopy(pstate, *argsp);
		parmp++;
	}

	// If we have too few arguments, use default values, if provided
	if (argsunder > 0) {
		if (((NameDclAstNode*)parmp->node)->value == NULL)
			errorMsgNode((AstNode*)node, ErrorFewArgs, "Function call requires more arguments than specified");
		else {
			while (argsunder--) {
				nodesAdd(&node->parms, ((NameDclAstNode*)parmp->node)->value);
				parmp++;
			}
		}
	}
	break;
}
}
}

// Create a new addr node
AddrAstNode *newAddrAstNode() {
	AddrAstNode *node;
	newAstNode(node, AddrAstNode, AddrNode);
	return node;
}

// Serialize addr
void addrPrint(AddrAstNode *node) {
	astFprint("&(");
	astPrintNode(node->vtype);
	astFprint("->");
	astPrintNode(node->exp);
	astFprint(")");
}

// Type check borrowed reference creator
void addrTypeCheckBorrow(AddrAstNode *node, PtrAstNode *ptype) {
	AstNode *exp = node->exp;
	if (exp->asttype != NameUseNode || ((NameUseAstNode*)exp)->dclnode->asttype != VarNameDclNode) {
		errorMsgNode((AstNode*)node, ErrorNotLval, "May only borrow from lvals (e.g., variable)");
		return;
	}
	if (!permMatches(ptype->perm, ((NameUseAstNode*)exp)->dclnode->perm))
		errorMsgNode((AstNode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");
}

// Analyze addr node
void addrPass(AstPass *pstate, AddrAstNode *node) {
	astPass(pstate, node->exp);
	if (pstate->pass == TypeCheck) {
		if (!isExpNode(node->exp)) {
			errorMsgNode(node->exp, ErrorBadTerm, "Needs to be an expression");
			return;
		}
		PtrAstNode *ptype = (PtrAstNode *)node->vtype;
		if (ptype->pvtype == NULL)
			ptype->pvtype = ((TypedAstNode *)node->exp)->vtype; // inferred
		if (ptype->alloc == voidType)
			addrTypeCheckBorrow(node, ptype);
		else
			allocAllocate(node, ptype);
	}
}