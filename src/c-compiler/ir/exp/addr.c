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
        lval = (INode *)((NameUseNode *)lval)->dclnode;
        // Fallthrough is expected here
    case FnDclTag:
    {
        if (lval->tag == VarDclTag)
            *lvalperm = ((VarDclNode *)lval)->perm;
        else
            *lvalperm = (INode*)opaqPerm; // Function
        return (INamedNode *)lval;
    }

    case DerefTag:
    {
        DerefNode *deref = (DerefNode*)lval;
        return addrGetVarPerm(deref->exp, lvalperm);
    }

    // An array element (obj[2]) or property access (obj.prop)
    case ArrIndexTag:
    case StrFieldTag:
    {
        FnCallNode *element = (FnCallNode *)lval;
        INamedNode *lvalvar = addrGetVarPerm(element->objfn, lvalperm);
        if (lvalvar == NULL)
            return NULL;
        INode *lvaltype = iexpGetTypeDcl((INode*)lvalvar);
        if (lvaltype->tag == RefTag) {
            *lvalperm = ((RefNode*)lvaltype)->perm; // implicit deref 
        }
        /*
        if (lvalperm = permIsLocked(*lvalperm) && var_is_not_unlocked) {
            errorMsgNode(lval, ErrorNotLval, "Cannot borrow a substructure reference from a locked variable");
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

    // One cannot borrow a reference from any other node
    default:
        errorMsgNode(lval, ErrorNotLval, "You can only borrow a reference from a variable, function, field, array element or array");
        return NULL;
    }
}

// Analyze addr node
void addrPass(PassState *pstate, AddrNode **nodep) {
    AddrNode *node = *nodep;
    if (pstate->pass == NameResolution) {
        inodeWalk(pstate, &node->exp);
        return;
    }

    RefNode *reftype = (RefNode *)node->vtype;

    /*
    // If we are borrowing a ref from indexing into a method-typed object
    // rewrite it as: (&obj).`&[]`()
    FnCallNode *index = (FnCallNode *)node->exp;
    if (index->tag == FnCallTag && (index->flags & FlagArrSlice) && reftype->alloc == voidType) {
        inodeWalk(pstate, &index->objfn);  // Unfortunately, we need to do this to infer type
        if (isMethodType(iexpGetTypeDcl(index->objfn))) {
            node->exp = index->objfn;
            index->objfn = (INode*)node;
            *nodep = (AddrNode*)node->exp;
            index->methprop->namesym = refIndexName;
            inodeWalk(pstate, &node->exp);
            return;
        }
    } */

    // Type check a borrowed reference
    inodeWalk(pstate, &node->exp);
    reftype->scope = 0;  // Presume lifetime scope is global

    INode *lval = node->exp;
    INode *lvalperm = (INode*)immPerm;
    if (lval->tag != StrLitTag) {
        // lval is the variable or variable sub-structure we want to get a reference to
        // From it, obtain variable we are borrowing from and actual/calculated permission
        INamedNode *lvalvar = addrGetVarPerm(lval, &lvalperm);
        if (lvalvar == NULL) {
            reftype->pvtype = (INode*)voidType;  // Just to avoid a compiler crash later
            return;
        }
        // Set lifetime of reference to borrowed variable's lifetime
        if (lvalvar->tag == VarDclTag) {
            reftype->scope = ((VarDclNode*)lvalvar)->scope;
        }
    }
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
}

// Perform data flow analysis on addr node
void addrFlow(FlowState *fstate, AddrNode **nodep) {
    AddrNode *node = *nodep;
    RefNode *reftype = (RefNode *)node->vtype;
    // Borrowed reference:  Deactivate source variable if necessary
}

// Insert automatic ref, if node is a variable
void addrAuto(INode **node, INode* reftype) {
    AddrNode *addrnode = newAddrNode();
    addrnode->exp = *node;
    addrnode->vtype = reftype;
    *node = (INode*)addrnode;
}
