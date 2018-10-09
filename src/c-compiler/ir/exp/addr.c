/** Handling for address-of expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new addr node
AddrNode *newAddrNode() {
	AddrNode *node;
	newNode(node, AddrNode, AddrTag);
	return node;
}

// Serialize addr
void addrPrint(AddrNode *node) {
	inodeFprint("&(");
	inodePrintNode(node->vtype);
	inodeFprint("->");
	inodePrintNode(node->exp);
	inodeFprint(")");
}

// Extract lval variable and overall permission from lval
INamedNode *addrGetVarPerm(INode *lval, INode **lvalperm) {
    switch (lval->tag) {

    // A variable or named function node
    case VarNameUseTag:
    {
        INamedNode *lvalvar = ((NameUseNode *)lval)->dclnode;
        if (lvalvar->tag == VarDclTag)
            *lvalperm = ((VarDclNode *)lvalvar)->perm;
        else
            *lvalperm = (INode*)idPerm; // Function
        return lvalvar;
    }

    // An array element (obj[2]) or property access (obj.prop)
    case FnCallTag:
    {
        FnCallNode *element = (FnCallNode *)lval;
        if (element->flags & FlagIndex || element->methprop) {
            INamedNode *lvalvar = addrGetVarPerm(element->objfn, lvalperm);
            if (lvalvar == NULL)
                return NULL;
            /*
            if (lvalperm = permIsLocked(*lvalperm) && var_is_not_unlocked) {
                errorMsgNode(lval, ErrorNotLval, "Cannot borrow a subtructure reference from a locked variable");
                return NULL
            } */
            if (element->methprop) {
                PermNode *methperm = (PermNode *)((VarDclNode*)((NameUseNode *)element->methprop)->dclnode)->perm;
                // Downgrade overall static permission if property is immutable
                if (methperm == immPerm)
                    *lvalperm = (INode*)constPerm;
            }
            return lvalvar;
        }
    }
    // Fall through to error message is intentional here for real function call

    // One cannot borrow a reference from any other node
    default:
        errorMsgNode(lval, ErrorNotLval, "You can only borrow a reference from a named variable or function");
        return NULL;
    }
}

// Type check borrowed reference creator
void addrTypeCheckBorrow(AddrNode *node, RefNode *reftype) {

    // lval is the variable or variable sub-structure we want to get a reference to
    // From it, obtain variable we are borrowing from and actual/calculated permission
    INode *lval = node->exp;
    INode *lvalperm = NULL;
    INamedNode *lvalvar = addrGetVarPerm(lval, &lvalperm);
    if (lvalvar == NULL)
        return;
    INode *lvaltype = ((ITypedNode*)lval)->vtype;

    // The reference's value type is unknown (NULL).
    // Let's infer type from the lval we are borrowing from
    if (lvaltype->tag == ArrayTag) {
        // Borrowing from a fixed size array creates an array reference
        reftype->pvtype = ((ArrayNode*)lvaltype)->elemtype;
        refSliceFatPtr(reftype);
    }
    else
        reftype->pvtype = lvaltype;

    // Ensure lval's permission matches requested permission
    if (reftype->perm) {
        if (!permMatches(reftype->perm, lvalperm))
            errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");
    }
    else
        reftype->perm = lvalperm;

    // Set lifetime of reference to borrowed variable's lifetime
    if (lvalvar->tag == VarDclTag) {
        reftype->scope = ((VarDclNode*)lvalvar)->scope;
        // Perform borrow logic on variable we are borrowing from
    }
    else
        reftype->scope = 0;  // Function's scope is global
}

// Type check allocator reference creator
void addrTypeCheckAlloc(AddrNode *node, RefNode *reftype) {

    // expression must be a value usable for initializing allocated reference
    INode *initval = node->exp;
    if (!isExpNode(initval)) {
        errorMsgNode(initval, ErrorBadTerm, "Needs to be a value");
        return;
    }

    // Infer reference's value type based on initial value
    reftype->pvtype = ((ITypedNode*)initval)->vtype;
}

// Analyze addr node
void addrPass(PassState *pstate, AddrNode *node) {
	inodeWalk(pstate, &node->exp);

	if (pstate->pass == TypeCheck) {
        // Type check a borrowed reference
        RefNode *reftype = (RefNode *)node->vtype;
        if (reftype->alloc == voidType)
            addrTypeCheckBorrow(node, reftype);
        else
            addrTypeCheckAlloc(node, reftype);
	}
}

// Perform data flow analysis on addr node
void addrFlow(FlowState *fstate, AddrNode **nodep) {
    AddrNode *node = *nodep;
    RefNode *reftype = (RefNode *)node->vtype;
    if (reftype->alloc != voidType) {
        // For an allocated reference, we need to handle the copied value
        flowLoadValue(fstate, &node->exp, 1);
    }
    else {
        // Borrowed reference:  Deactivate source variable if necessary
    }
}