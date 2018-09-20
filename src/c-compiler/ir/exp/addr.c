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

// Type check borrowed reference creator
void addrTypeCheckBorrow(AddrNode *node, RefNode *reftype) {

    // lval is the variable or variable sub-structure we want to get a reference to
    INode *lval = node->exp;
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

    // Obtain variable we are borrowing from and actual permission
    if (!iexpIsLval(lval)) {
		errorMsgNode((INode*)node, ErrorNotLval, "May only borrow from lvals (e.g., variable)");
		return;
	}
    INamedNode *dclnode = ((NameUseNode*)lval)->dclnode;
    INode *permlval = (dclnode->tag == VarDclTag) ? ((VarDclNode*)dclnode)->perm : (INode*)immPerm;

    // Ensure actual permission matches requested permission
	if (!permMatches(reftype->perm, permlval))
		errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");

    // Set lifetime of reference to borrowed variable's lifetime
    // Perform borrow logic on variable we are borrowing from
}

// Type check allocator reference creator
void addrTypeCheckAlloc(AddrNode *node, RefNode *reftype) {
    // Type check an allocated reference
    if (!isExpNode(node->exp)) {
        errorMsgNode(node->exp, ErrorBadTerm, "Needs to be an expression");
        return;
    }
    if (reftype->pvtype == NULL) {
        // Infer reference's value type
        INode *exptype = ((ITypedNode*)node->exp)->vtype;
        if (exptype->tag == ArrayTag) {
            reftype->pvtype = ((ArrayNode*)exptype)->elemtype;
            refSliceFatPtr(reftype);
        }
        else
            reftype->pvtype = exptype;
    }

    // inodeWalk(pstate, &node->vtype); // Type check the reference type

    allocAllocate(node, reftype);
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