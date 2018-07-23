/** AST handling for expression nodes that might do value copies/moves
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"
#include "../shared/error.h"

#include <assert.h>

void handleCopy(PassState *pstate, AstNode *node) {

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
	case VarNameUseTag:
	case DerefNode:
	case DotOpNode:
		return 1;
	// future:  [] indexing and .member
	default: break;
	}

	return 0;
}

// Analyze assignment node
void assignPass(PassState *pstate, AssignAstNode *node) {
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
	node->args = newNodes(nnodes);
	return node;
}

// Serialize function call node
void fnCallPrint(FnCallAstNode *node) {
	AstNode **nodesp;
	uint32_t cnt;
	astPrintNode(node->fn);
	astFprint("(");
	for (nodesFor(node->args, cnt, nodesp)) {
		astPrintNode(*nodesp);
		if (cnt > 1)
			astFprint(", ");
	}
	astFprint(")");
}

// Analyze function call node
void fnCallPass(PassState *pstate, FnCallAstNode *node) {
	AstNode **argsp;
	uint32_t cnt;
	for (nodesFor(node->args, cnt, argsp))
        astPass(pstate, *argsp);
    astPass(pstate, node->fn);

    switch (pstate->pass) {
    case TypeCheck:
    {
	    // If this is an object call, resolve method name within first argument's type
	    if (node->fn->asttype == MbrNameUseTag) {
		    NameUseAstNode *methname = (NameUseAstNode*)node->fn;
		    Name *methsym = methname->namesym;
		    FnDclAstNode *method;
            if (methsym->namestr == '_') {
                errorMsgNode((AstNode*)node, ErrorNotPublic, "May not access the private method `%s`.", &methsym->namestr);
                return;
            }
		    else if (method = methtypeFindMethod(typeGetVtype(*nodesNodes(node->args)), methsym, node->args, (AstNode*)node)) {
			    methname->asttype = VarNameUseTag;
			    methname->dclnode = (NamedAstNode*)method;
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
	    if (fnsig->asttype == FnSigType)
		    node->vtype = ((FnSigAstNode*)fnsig)->rettype;
	    else {
		    errorMsgNode(node->fn, ErrorNotFn, "Cannot call a value that is not a function");
		    return;
	    }

	    // Error out if we have too many arguments
	    int argsunder = ((FnSigAstNode*)fnsig)->parms->used - node->args->used;
	    if (argsunder < 0) {
		    errorMsgNode((AstNode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
		    return;
	    }

	    // Type check that passed arguments match declared parameters
	    AstNode **argsp;
	    uint32_t cnt;
	    AstNode **parmp = &nodesGet(((FnSigAstNode*)fnsig)->parms, 0);
	    for (nodesFor(node->args, cnt, argsp)) {
		    if (!typeCoerces(*parmp, argsp))
			    errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
		    else
			    handleCopy(pstate, *argsp);
		    parmp++;
	    }

	    // If we have too few arguments, use default values, if provided
	    if (argsunder > 0) {
		    if (((VarDclAstNode*)*parmp)->value == NULL)
			    errorMsgNode((AstNode*)node, ErrorFewArgs, "Function call requires more arguments than specified");
		    else {
			    while (argsunder--) {
				    nodesAdd(&node->args, ((VarDclAstNode*)*parmp)->value);
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
	if (exp->asttype != VarNameUseTag
        || (((NameUseAstNode*)exp)->dclnode->asttype != VarDclTag
            && ((NameUseAstNode*)exp)->dclnode->asttype != FnDclTag)) {
		errorMsgNode((AstNode*)node, ErrorNotLval, "May only borrow from lvals (e.g., variable)");
		return;
	}
    NamedAstNode *dclnode = ((NameUseAstNode*)exp)->dclnode;
    PermAstNode *perm = (dclnode->asttype == VarDclTag) ? ((VarDclAstNode*)dclnode)->perm : immPerm;
	if (!permMatches(ptype->perm, perm))
		errorMsgNode((AstNode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");
}

// Analyze addr node
void addrPass(PassState *pstate, AddrAstNode *node) {
	astPass(pstate, node->exp);
	if (pstate->pass == TypeCheck) {
		if (!isValueNode(node->exp)) {
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