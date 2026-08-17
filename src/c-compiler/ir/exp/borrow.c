/** Handling for borrow expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a borrowed ref node
INode *newBorrowMutRef(INode *node, INode* type, INode *perm) {
    RefNode *reftype = type != unknownType ? newRefNodeFull(RefTag, node, borrowRef, perm, type) : (RefNode*)unknownType;
    RefNode *borrownode = newRefNodeFull(BorrowTag, node, borrowRef, perm, node);
    borrownode->vtype = (INode*)reftype;
    return (INode*)borrownode;
}

// Inject a typed, borrowed node on some node (expected to be an lval)
void borrowMutRef(INode **nodep, INode* type, INode *perm) {
    INode *node = *nodep;
    // Rather than borrow from a deref, just return the ptr node we are de-reffing
    if (node->tag == DerefTag) {
        StarNode *derefnode = (StarNode *)node;
        *nodep = derefnode->vtexp;
        return;
    }
  
    if (iexpIsLvalError(node) == 0) {
        errorMsgNode(node, ErrorInvType, "Auto-borrowing can only be done on an lval");
    }

    // Verify lval is mutable
    INode *lvalperm = (INode*)immPerm;
    uint16_t scope = 0;
    INode *lvalvar = iexpGetLvalInfo(node, &lvalperm, &scope);
    if (!permMatches(perm, lvalperm))
        errorMsgNode((INode *)node, ErrorBadPerm, "Cannot borrow mutable reference to this.");

    RefNode *reftype = type != unknownType? newRefNodeFull(RefTag, node, borrowRef, perm, type) : (RefNode*)unknownType;
    RefNode *borrownode = newRefNodeFull(BorrowTag, node, borrowRef, perm, node);
    borrownode->vtype = (INode*)reftype;
    *nodep = (INode*)borrownode;
}

// Auto-inject a borrow note in front of 'from', to create totypedcl type
void borrowAuto(INode **from, INode *totypedcl) {
    // Borrow from array to create arrayref (only one supported currently)
    RefNode *arrreftype = (RefNode*)totypedcl;
    RefNode *addrtype = newRefNodeFull(ArrayRefTag, *from, borrowRef, newPermUseNode(roPerm), arrreftype->vtexp);
    RefNode *borrownode = newRefNode(ArrayBorrowTag);
    borrownode->vtype = (INode*)addrtype;
    borrownode->vtexp = *from;
    *from = (INode*)borrownode;
}

// Can we safely auto-borrow to match expected type?
// Note: totype has already done GetTypeDcl
int borrowAutoMatches(INode *from, RefNode *totype) {
    // We can only borrow from an lval
    if (!iexpIsLval(from))
        return 0;
    INode *fromtype = iexpGetTypeDcl(from);

    // Handle auto borrow of array to obtain a borrowed array reference (slice)
    if (totype->tag == ArrayRefTag && fromtype->tag == ArrayTag) {
        return (itypeIsSame(((RefNode*)totype)->vtexp, arrayElemType(fromtype))
            && itypeGetTypeDcl(totype->perm) == (INode*)roPerm && itypeGetTypeDcl(totype->region) == borrowRef);
    }
    return 0;
}

// Serialize borrow node
void borrowPrint(RefNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->vtype);
    inodeFprint("->");
    inodePrintNode(node->vtexp);
    inodeFprint(")");
}

// Answer whether '&[]value' means the value type's own whole-value '&[]' method
// rather than a slice over the value itself.
//
// A type may give '&[]' its own meaning, and the indexed form already honors it:
// '&mut v[i]' parses to a FlagIndex|FlagBorrow call, which fnCallTypeCheck names
// '&[]' and dispatches like any other method. The whole-value form parses to this
// borrow node instead and never becomes a call, so it never reached dispatch.
//
// The probe alters nothing and reports nothing. A type declaring no such method,
// or none whose 'self' accepts the receiver this borrow would make, keeps the
// borrow's own meaning -- over a non-array that is a one-element slice, which is
// deliberate.
static int borrowRefIndexDispatches(RefNode *node) {
    INode *lvaltype = iexpGetTypeDcl(node->vtexp);
    if (!isMethodType(lvaltype))
        return 0;
    INode *found = iNsTypeFindFnField((INsTypeNode*)lvaltype, refIndexName);
    if (found == NULL || !(found->flags & FlagMethFld)
        || (found->tag != FnDclTag && found->tag != FnOverloadDclTag))
        return 0;

    // Probe with the receiver the borrow is about to become, which is the
    // permission the borrow's own inference below would settle on.
    INode *perm = node->perm != unknownType ? node->perm
        : newPermUseNode(itypeIsConcrete(lvaltype) ? roPerm : opaqPerm);
    INode *recvr = newBorrowMutRef(node->vtexp, lvaltype, perm);
    enum OverloadMatch status;
    return iNsTypeFindMethod(found, &recvr, NULL, &status) != NULL;
}

// Answer whether '&v[i]' should be re-associated to '(&v)[i]'.
//
// A borrow reaches the whole suffixed term, so the parser hands '&v[i]' over as a
// borrow of the indexed element. That is the meaning, but it is not the shape the
// index wants: a type may declare its own '`&[]`', and that method decides what a
// reference to one of its elements is -- taking the very receiver this borrow
// would make. Reading past it to the by-value '`[]`' would borrow a temporary.
//
// So the index takes the borrow as its receiver and becomes the whole expression,
// which is also the shape fnCallArrIndex has always wanted for an array, a slice
// or a pointer. Only an index directly on the term qualifies: '&x[i].a' borrows
// the field, and its index is a plain one.
//
// A type in that position is not a receiver and is left alone. '&Box[i64]' is an
// instantiation and never arrives here at all, refNameRes having kept the whole
// node a type. '&Point[1, 2]' does arrive, and stays a literal of Point rather
// than becoming an index of '&Point' -- a temporary, which the borrow refuses
// below. It used to be a literal of the reference type, which nothing accepted.
static int borrowReassocIndex(RefNode *node) {
    if (node->tag != BorrowTag || node->vtexp->tag != FnCallTag)
        return 0;
    FnCallNode *index = (FnCallNode*)node->vtexp;
    return (index->flags & FlagIndex) && index->methfld == NULL && !isTypeNode(index->objfn);
}

// Analyze borrow node
void borrowTypeCheck(TypeCheckState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;

    if (borrowReassocIndex(node)) {
        FnCallNode *index = (FnCallNode*)node->vtexp;
        node->vtexp = index->objfn;
        node->flags |= FlagSuffix;  // the borrow is now the inner link of an index chain
        index->objfn = (INode*)node;
        index->flags |= FlagBorrow;
        *((INode**)nodep) = (INode*)index;
        inodeTypeCheckAny(pstate, (INode**)nodep);
        return;
    }

    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0)
        return;

    // Every operand a borrow refuses is refused for one reason, so the borrow says
    // that reason rather than iexpIsLvalError's "must be lval", which explains an
    // assignment target and not this. A borrow reaches the whole suffixed term, so
    // '&p.sum()' is the call's result -- a temporary -- and '(&p).sum()' is how a
    // method is called on a borrowed receiver.
    if (!iexpIsLval(node->vtexp)) {
        errorMsgNode(node->vtexp, ErrorBadLval,
            "May not borrow a temporary value. A borrowed reference needs a place in memory to point at.");
        return;
    }

    // Where '&[]value' dispatches to the value's own '&[]' method, the receiver
    // that method wants is a plain borrow of the value. Retag to build exactly
    // that, so the lval, permission and lifetime checks below are the ones a
    // hand-written '&mut value' gets, and wrap the result in the call afterward.
    // A borrow carrying suffixes is a link in a chain -- '&[]mut v.field' borrows
    // the field -- and is not the whole-value form.
    int dispatchRefIndex = node->tag == ArrayBorrowTag && !(node->flags & FlagSuffix)
        && borrowRefIndexDispatches(node);
    if (dispatchRefIndex)
        node->tag = BorrowTag;

    // Auto-deref the exp, if we are borrowing a reference to a reference's field or indexed value
    INode *exptype = iexpGetTypeDcl(node->vtexp);
    if ((node->flags & FlagSuffix) && (exptype->tag == RefTag || exptype->tag == PtrTag || exptype->tag == ArrayRefTag)) {
        StarNode *deref = newStarNode(DerefTag);
        deref->vtexp = node->vtexp;
        if (exptype->tag == ArrayRefTag)
            deref->vtype = (INode*)newArrayDerefNodeFrom((RefNode*)exptype);
        else
            deref->vtype = ((RefNode*)exptype)->vtexp;  // assumes StarNode has field in same place
        node->vtexp = (INode*)deref;
    }

    // Setup lval, perm and scope info as if we were borrowing from a global constant literal.
    // If not, extract this info from expression nodes
    uint16_t scope = 0;  // global
    INode *lval = node->vtexp;
    INode *lvalperm = (INode*)immPerm;
    scope = 0;  // Global
    if (lval->tag != StringLitTag) {
        // lval is the variable or variable sub-structure we want to get a reference to
        // From it, obtain variable we are borrowing from and actual/calculated permission
        INode *lvalvar = iexpGetLvalInfo(lval, &lvalperm, &scope);
        if (lvalvar == NULL) {
            node->vtype = (INode*)newRefNodeFull(RefTag, (INode*)node, node->region, node->perm, (INode*)unknownType); // To avoid a crash later
            return;
        }
        // Set lifetime of reference to borrowed variable's lifetime
        if (lvalvar->tag == VarDclTag)
            scope = ((VarDclNode*)lvalvar)->scope;
    }
    INode *lvaltype = ((IExpNode*)lval)->vtype;

    // The reference's value type is currently unknown
    // Let's infer this value type from the lval we are borrowing from
    uint16_t tag;
    INode *refvtype;
    if (node->tag == BorrowTag) {
        tag = RefTag;
        refvtype = lvaltype;
    }
    else {
        tag = ArrayRefTag;  // Borrowing to create an array reference
        if (lvaltype->tag == ArrayTag) {
            refvtype = arrayElemType(lvaltype);
        }
        else if (lvaltype->tag == ArrayDerefTag) {
            refvtype = ((RefNode*)lvaltype)->vtexp;
        }
        else
            refvtype = lvaltype;  // a one-element slice!
    }

    // Ensure requested/inferred permission matches lval's permission
    INode *refperm = node->perm;
    if (refperm == unknownType)
        refperm = newPermUseNode(itypeIsConcrete(refvtype) ? roPerm : opaqPerm);
    if (!permMatches(refperm, lvalperm))
        errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");

    RefNode *reftype = newRefNodeFull(tag, (INode*)node, borrowRef, refperm, refvtype);
    reftype->scope = scope;
    node->vtype = (INode *)reftype;

    // The borrowed receiver is now typed, so the method call can be selected
    // against it, exactly as '(&mut value).`&[]`()' is.
    if (dispatchRefIndex) {
        FnCallNode *call = newFnCallOpnameLower((INode*)node, (INode*)node, refIndexName, 0);
        fnCallLowerMethod(call);
        *nodep = (RefNode*)call;
    }
}

// Perform data flow analysis on addr node
void borrowFlow(FlowState *fstate, RefNode **nodep) {
    RefNode *node = *nodep;
    RefNode *reftype = (RefNode *)node->vtype;
    // Borrowed reference:  Deactivate source variable if necessary
}
