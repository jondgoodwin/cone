/** Handling for borrow expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a borrowed ref node
// The type an injected borrow of 'node' points at, given as 'type', often a
// declaration: where the value's own type is a use of it naming invariant
// lifetimes, the use, so that the borrow keeps the brands the value carries
static INode *borrowPointee(INode *node, INode *type) {
    if (!lifeInvariantSeen || type == unknownType || !isExpNode(node))
        return type;
    INode *use = ((IExpNode*)node)->vtype;
    if (use != type && use != NULL && itypeGetTypeDcl(use) == itypeGetTypeDcl(type) && lifeTypeHasBrands(use))
        return use;
    return type;
}

INode *newBorrowMutRef(INode *node, INode* type, INode *perm) {
    type = borrowPointee(node, type);
    RefNode *reftype = type != unknownType ? newRefNodeFull(RefTag, node, borrowRef, perm, type) : (RefNode*)unknownType;
    RefNode *borrownode = newRefNodeFull(BorrowTag, node, borrowRef, perm, node);
    borrownode->vtype = (INode*)reftype;
    return (INode*)borrownode;
}

// Refuse a borrow of a place within a named constant's own value, reporting it
// once, where the whole constant array or string (borrowIsConstLit) was not
// what was borrowed. A constant is its literal wherever it is used (refterm.html,
// "Named constants"), so '&K' for 'const K = 5' is '&5', a temporary, and so is
// a borrow of a field or element of any constant. A place reached through a
// reference the constant holds is where that reference points, and is not
// refused here.
static int borrowRefusesConst(INode *place) {
    INode *node = place;
    while (node->tag == FldAccessTag || node->tag == ArrIndexTag) {
        INode *obj = ((FnCallNode*)node)->objfn;
        uint16_t objtag = iexpGetTypeDcl(obj)->tag;
        if (objtag == RefTag || objtag == ArrayRefTag || objtag == PtrTag || objtag == VirtRefTag)
            return 0;
        node = obj;
    }
    if (!nameUseNames(node, ConstDclTag))
        return 0;
    errorMsgNode(place, ErrorBadLval,
        "May not borrow from the named constant %s here. Each use of a constant is a fresh copy of its value, with no place in memory to point at; only a whole constant array or string is kept in one, and borrowed there.",
        &((ConstDclNode*)nameUseGetDcl((NameUseNode*)node))->namesym->namestr);
    return 1;
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
    // A constant is refused as a temporary; only a mutable borrow of one is
    // refused below, as a borrow of something not mutable
    int refused = permMatches(perm, (INode*)immPerm) && borrowRefusesConst(node);
    if (!refused && !permMatches(perm, lvalperm)) {
        if (lvalvar && lvalvar->tag == VarDclTag)
            errorMsgNode((INode *)node, ErrorBadPerm, "Cannot borrow a mutable reference to `%s`, which is not mutable",
                &((VarDclNode *)lvalvar)->namesym->namestr);
        else
            errorMsgNode((INode *)node, ErrorBadPerm, "Cannot borrow a mutable reference to a value that is not mutable");
    }

    // The lval's scope is the borrow's lifetime, as borrowTypeCheck records it
    // for a borrow written in source. This node is never type checked, so
    // nothing else would set it, and newRefNode's default means global.
    RefNode *reftype = (RefNode*)unknownType;
    if (type != unknownType) {
        reftype = newRefNodeFull(RefTag, node, borrowRef, perm, borrowPointee(node, type));
        reftype->scope = scope;
    }
    RefNode *borrownode = newRefNodeFull(BorrowTag, node, borrowRef, perm, node);
    borrownode->vtype = (INode*)reftype;
    *nodep = (INode*)borrownode;
}

void borrowTempRef(INode **nodep, INode *type, INode *perm, uint16_t scope) {
    INode *node = *nodep;
    RefNode *reftype = newRefNodeFull(RefTag, node, borrowRef, perm, type);
    reftype->scope = scope;
    RefNode *borrownode = newRefNodeFull(BorrowTag, node, borrowRef, perm, node);
    borrownode->vtype = (INode*)reftype;
    *nodep = (INode*)borrownode;
}

// Auto-inject a borrow note in front of 'from', to create totypedcl type
void borrowAuto(INode **from, INode *totypedcl) {
    // Borrow from array to create arrayref (only one supported currently)
    RefNode *arrreftype = (RefNode*)totypedcl;
    RefNode *addrtype = newRefNodeFull(ArrayRefTag, *from, borrowRef, newPermUseNode(roPerm), arrreftype->vtexp);
    // The slice lives as long as the array it borrows, as '&[]arr' written out
    // would; borrowAutoMatches has already established that 'from' is an lval
    INode *lvalperm = (INode*)immPerm;
    uint16_t scope = 0;
    iexpGetLvalInfo(*from, &lvalperm, &scope);
    addrtype->scope = scope;
    RefNode *borrownode = newRefNode(ArrayBorrowTag);
    inodeLexCopy((INode*)borrownode, *from);
    borrownode->vtype = (INode*)addrtype;
    borrownode->vtexp = *from;
    *from = (INode*)borrownode;
}

// Is 'from' a '&uni' reference held in a place, wanted as a borrowed reference
// that may be shared ('&', '&mut', '&imm' ...)? Handing it over lends it rather
// than moving it: "a uni can be borrowed as mut or imm. After the borrowed
// references' last use, you once again have the original uni reference"
// (refperm.html, "Borrowed reference recovery"). A '&uni' wanted as a '&uni'
// still moves. Note: totypedcl has already done GetTypeDcl
int borrowUniReborrows(INode *from, INode *totypedcl) {
    RefNode *fromtype = (RefNode*)iexpGetTypeDcl(from);
    RefNode *totype = (RefNode*)totypedcl;
    return fromtype->tag == RefTag && fromtype->region == borrowRef
        && itypeGetTypeDcl(fromtype->perm) == (INode*)uniPerm
        && totype->tag == RefTag && totype->region == borrowRef && !itypeIsMove(totypedcl)
        && iexpIsLval(from);
}

// Rewrite the reference 'from' to the borrow '&perm *from', typed as a borrowed
// reference to 'vtexp', as borrowTypeCheck would build it if written out: the
// lifetime of a borrow of '*from' (iexpGetLvalInfo). The permission has already
// been checked by the match that asked for it.
static void borrowDerefOf(INode **from, INode *perm, INode *vtexp) {
    StarNode *deref = newStarNode(DerefTag);
    inodeLexCopy((INode*)deref, *from);
    deref->vtexp = *from;
    deref->vtype = ((RefNode*)iexpGetTypeDcl(*from))->vtexp;

    INode *lvalperm = (INode*)immPerm;
    uint16_t scope = 0;
    iexpGetLvalInfo((INode*)deref, &lvalperm, &scope);

    RefNode *reftype = newRefNodeFull(RefTag, *from, borrowRef, perm, vtexp);
    reftype->scope = scope;
    RefNode *borrownode = newRefNodeFull(BorrowTag, *from, borrowRef, perm, (INode*)deref);
    borrownode->vtype = (INode*)reftype;
    *from = (INode*)borrownode;
}

// Lend a '&uni' reference as the borrowed reference 'totypedcl' wants, by
// rewriting it to the reborrow '&mut *from' that borrowTypeCheck would build
// if written out: the same permission check, and the lifetime of the variable
// the reference is held in. Flow analysis then sees a borrow of '*from', which
// freezes the reference while the borrow is used, and not a move of it.
void borrowUniReborrow(INode **from, INode *totypedcl) {
    RefNode *totype = (RefNode*)totypedcl;
    borrowDerefOf(from, totype->perm, totype->vtexp);
}

// Is 'from' an owning reference that is the only holder of its value (a 'So',
// a 'Rc' still 'uni'), held in a place, wanted as a borrowed reference that
// may not be shared ('&uni')? An owning reference coerced to a borrowed one is
// borrowed from, whatever the borrow's permission: the owner stays, frozen
// while the borrow is used, and still ends its value at its own scope's end
// (refborref.html, "Borrowing from another reference"; refperm.html, "From
// 'uni'"). A coercion to '&' or '&mut' is a recast that flow analysis already
// sees as that borrow (pwOwnedLent); one to a '&uni' is a move type, which would
// move the owner into a reference that ends nothing. Note: totypedcl has
// already done GetTypeDcl
int borrowOwnerLendsUni(INode *from, INode *totypedcl) {
    RefNode *fromtype = (RefNode*)iexpGetTypeDcl(from);
    RefNode *totype = (RefNode*)totypedcl;
    return fromtype->tag == RefTag && itypeGetTypeDcl(fromtype->region) != borrowRef
        && itypeIsMove((INode*)fromtype)
        && totype->tag == RefTag && totype->region == borrowRef && itypeIsMove(totypedcl)
        && iexpIsLval(from);
}

// Lend such an owning reference by rewriting it to the borrow '&uni *from',
// typed as a borrow of what the owner points at. The caller then coerces that
// borrow to the type wanted, which may still be a recast (an enrichment's base).
void borrowOwnerLend(INode **from, INode *totypedcl) {
    RefNode *totype = (RefNode*)totypedcl;
    borrowDerefOf(from, totype->perm, ((RefNode*)iexpGetTypeDcl(*from))->vtexp);
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
    INode *found = aliasDclResolve(iNsTypeFindFnField((INsTypeNode*)lvaltype, refIndexName));
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
// node a type. A literal in brackets, '&Some[x]' (or a struct's, '&Point[1, 2]',
// refused as one), does arrive, and stays a literal rather than becoming an
// index of '&Some' -- a temporary, which the borrow refuses below. It used to
// be a literal of the reference type, which nothing accepted.
// Nor is a list of type arguments an index: '&half[i64]' and
// '&Holder.pick[i32]' borrow the instance they name, which re-associating would
// strip of its type arguments, leaving a type to index '&half' by.
static int borrowReassocIndex(RefNode *node) {
    if (node->tag != BorrowTag || node->vtexp->tag != FnCallTag)
        return 0;
    FnCallNode *index = (FnCallNode*)node->vtexp;
    return (index->flags & FlagIndex) && index->methfld == NULL && !isTypeNode(index->objfn)
        && !fnCallHasTypeArgs(index);
}

// Answer whether a borrow's operand is a literal constant with storage of its
// own: a string literal, or an array literal whose elements are all constants
// ('&[1, 2, 3]'). Generation places either in a constant global, so it is
// borrowed as a global constant is: for as long as the program runs, and
// immutably. An array literal with a computed element has no such place. A
// named constant is its literal wherever it is used (refterm.html, "Named
// constants"), so '&K' is borrowed as its literal would be.
int borrowIsConstLit(INode *node) {
    while (nameUseNames(node, ConstDclTag))
        node = ((ConstDclNode*)nameUseGetDcl((NameUseNode*)node))->value;
    return node->tag == StringLitTag
        || (node->tag == ArrayLitTag && arrayLitIsLiteral((ArrayNode*)node));
}

// Is this a reference type whose referent is not the reference's own: a
// borrowed reference, or a pointer?
static int borrowRefersAway(INode *typedcl) {
    if (typedcl->tag == PtrTag)
        return 1;
    return (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag || typedcl->tag == VirtRefTag)
        && itypeGetTypeDcl(((RefNode*)typedcl)->region) == borrowRef;
}

INode **borrowTempRoot(INode **nodep) {
    while (1) {
        INode *node = *nodep;
        if (isNameUseNode(node) || borrowIsConstLit(node))
            return NULL;
        switch (node->tag) {
        case FldAccessTag:
        case ArrIndexTag:
            nodep = &((FnCallNode*)node)->objfn;
            if (borrowRefersAway(iexpGetTypeDcl(*nodep)))
                return NULL;
            break;
        case DerefTag:
            nodep = &((StarNode*)node)->vtexp;
            if (borrowRefersAway(iexpGetTypeDcl(*nodep)))
                return NULL;
            break;
        case CastTag:
            if (node->flags & FlagConvert)
                return nodep;
            nodep = &((CastNode*)node)->exp;
            break;
        default:
            return nodep;
        }
    }
}

// Retype a borrowed constant array literal to the reference type it is wanted
// as, when their element types differ: '&[1, 2, 3]' wanted as a '&[]u32'. The
// literal was typed from its elements alone, since a borrow passes no expected
// type to what it borrows, so its untyped number literals settled on i32. Coercing
// each element to the wanted element type is what arrayLitCoerce does for the
// literal unborrowed. Return 0, having changed nothing, where the borrow is not
// of a constant array literal or the wanted type is not a reference to an
// array or a slice; arrayLitCoerce returns 0 where an element does not coerce.
int borrowConstLitCoerce(INode *from, INode *totypedcl) {
    if ((from->tag != BorrowTag && from->tag != ArrayBorrowTag)
        || (totypedcl->tag != RefTag && totypedcl->tag != ArrayRefTag))
        return 0;
    RefNode *borrow = (RefNode*)from;
    INode *lit = borrow->vtexp;
    RefNode *fromtype = (RefNode*)borrow->vtype;
    if (lit->tag != ArrayLitTag || !arrayLitIsLiteral((ArrayNode*)lit)
        || fromtype->tag != (from->tag == BorrowTag ? RefTag : ArrayRefTag))
        return 0;
    INode *littype = ((IExpNode*)lit)->vtype;
    if (littype->tag != ArrayTag)
        return 0;

    // The array type wanted: a reference's own, or one of the literal's count
    // holding the slice's element type
    INode *wanted;
    if (totypedcl->tag == RefTag)
        wanted = itypeGetTypeDcl(((RefNode*)totypedcl)->vtexp);
    else
        wanted = (INode*)newArrayNodeTyped(lit, (size_t)arrayDim1(littype), ((RefNode*)totypedcl)->vtexp);
    if (wanted->tag != ArrayTag || !arrayLitCoerce((ArrayNode*)lit, wanted))
        return 0;

    // The borrow's type is rebuilt around the retyped literal, keeping its
    // permission and its lifetime, as borrowTypeCheck built it
    littype = ((IExpNode*)lit)->vtype;
    RefNode *reftype = newRefNodeFull(fromtype->tag, from, borrowRef, fromtype->perm,
        from->tag == BorrowTag ? littype : arrayElemType(littype));
    reftype->scope = fromtype->scope;
    borrow->vtype = (INode*)reftype;
    return 1;
}

uint16_t borrowTempScope(TypeCheckState *pstate) {
    return (uint16_t)pstate->scope;
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

    // A borrow reaches the whole suffixed term, so '&p.sum()' borrows the
    // call's result -- a temporary -- and '(&p).sum()' is how a method is
    // called on a borrowed receiver. A constant literal is not a temporary: it
    // has a place in a constant global. An array literal's elements computed
    // from constants alone are constants too, folded here into the literals
    // they compute (litFoldConst), so '&[R | G, B]' is one.
    if (node->vtexp->tag == ArrayLitTag)
        litFoldConst(&node->vtexp);
    // A place rooted in a temporary, borrowed in a local's initializer, may be
    // one Rust's rule extends to the end of the block: the temporary becomes a
    // hidden local of the block, kept if the borrow turns out to extend it
    // (vardcl.c, "Temporaries an initializer extends"). Any other is borrowed
    // where it is, and lives to the end of its statement, as Rust's does: flow
    // keeps it in a slot (flowTempBorrowed), and the loan walk refuses a borrow
    // of it used after that.
    INode **temp = borrowTempRoot(&node->vtexp);
    if (temp)
        varDclExtendTemp(pstate, temp);

    // An inline function has no code of its own: generation copies its body
    // into each caller and emits no symbol, so a reference to it would point at
    // nothing. An anonymous 'inline' function arrives the same way, as a name
    // use of the lifted declaration. The reference type is still built below,
    // so the rest of the function type checks without follow-on noise. Nor has
    // an intrinsic: the compiler expands its meaning, or its fallback body, at
    // each call.
    if (nameUseNames(node->vtexp, FnDclTag)
        && (((FnDclNode*)nameUseGetDcl((NameUseNode*)node->vtexp))->dclinfo.facts & DclIntrinsic)) {
        errorMsgNode(node->vtexp, ErrorInlineRef,
            "May not borrow a reference to an intrinsic. The compiler expands what it does at each call, so it has no code of its own to point at.");
    }
    else if (nameUseNames(node->vtexp, FnDclTag)
        && (nameUseGetDcl((NameUseNode*)node->vtexp)->flags & FlagInline)) {
        errorMsgNode(node->vtexp, ErrorInlineRef,
            "May not borrow a reference to an inline function. Its body is copied into each caller, so it has no code of its own to point at.");
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
    int refused = 0;
    if (!borrowIsConstLit(lval)) {
        // lval is the variable or variable sub-structure we want to get a reference to
        // From it, obtain variable we are borrowing from and actual/calculated permission
        // A place reached through a reference held in no variable -- the one
        // a call returned, '&id(&x).n' or '&*id(&x)' -- has no variable at its
        // root, but is still a place: it is where that reference points, and
        // lives as long as the reference's type says (iexpScopeThroughRef), as
        // a place reached through a reference held in a variable does. A place
        // rooted in a temporary itself is reached through nothing but this
        // borrow ('uni', unless an owner or a field on the way says less), and
        // lives no longer than the block it is made in (borrowTempScope).
        lvalperm = (INode*)uniPerm;
        INode *lvalvar = iexpGetLvalInfo(lval, &lvalperm, &scope);
        if (lvalvar == NULL && borrowTempRoot(&node->vtexp) != NULL)
            scope = borrowTempScope(pstate);
        // Refused once; the reference is still typed, so its uses check quietly
        refused = borrowRefusesConst(lval);
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
    if (!refused && !permMatches(refperm, lvalperm))
        errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");

    RefNode *reftype = newRefNodeFull(tag, (INode*)node, borrowRef, refperm, refvtype);
    reftype->scope = scope;
    node->vtype = (INode *)reftype;

    // The borrowed receiver is now typed, so the method call can be selected
    // against it, exactly as '(&mut value).`&[]`()' is.
    if (dispatchRefIndex) {
        FnCallNode *call = newFnCallOpnameLower((INode*)node, (INode*)node, refIndexName, 0);
        fnCallLowerMethod(pstate, call);
        *nodep = (RefNode*)call;
    }
}

// Walk the place a borrow points at, checking that its value was not moved out.
// The place itself is not read, so a reference the place is reached through is
// loaded as a value (a pointer) rather than read through: the variable at the
// root must not be moved out or hollowed. It may be uninitialized, so that a
// method taking it '&mut' can fill it in.
static void borrowFlowPlace(FlowState *fstate, INode **placep) {
    INode *place = *placep;
    if (isNameUseNode(place) && isExpNode(place)) {
        nameuseFlowBorrowed(fstate, (NameUseNode**)placep);
        return;
    }
    // A place rooted in a temporary -- '&make()', '&make().x', '&*make()',
    // which a method borrowing its receiver builds -- borrows it until its
    // statement's end (flowTempRead, flowTempBorrowed)
    switch (place->tag) {
    case DerefTag:
        flowLoadValue(fstate, &((StarNode *)place)->vtexp);
        flowTempRead(&((StarNode *)place)->vtexp);
        break;
    case FldAccessTag:
    case ArrIndexTag:
    {
        FnCallNode *access = (FnCallNode *)place;
        uint16_t objtag = iexpGetTypeDcl(access->objfn)->tag;
        if (objtag == RefTag || objtag == ArrayRefTag || objtag == PtrTag || objtag == VirtRefTag) {
            flowLoadValue(fstate, &access->objfn);
            flowTempRead(&access->objfn);
        }
        else
            borrowFlowPlace(fstate, &access->objfn);
        if (place->tag == ArrIndexTag) {
            INode **argsp;
            uint32_t cnt;
            for (nodesFor(access->args, cnt, argsp))
                flowLoadValue(fstate, argsp);
        }
        break;
    }
    case CastTag:
        if (!(place->flags & FlagConvert)) {
            borrowFlowPlace(fstate, &((CastNode *)place)->exp);
            break;
        }
        flowLoadValue(fstate, placep);
        flowTempBorrowed(placep);
        break;
    // A temporary borrowed, or a part of one: kept in a slot to its statement's end
    default:
        flowLoadValue(fstate, placep);
        flowTempBorrowed(placep);
        break;
    }
}

// Perform data flow analysis on a borrow: what it borrows must not be moved out.
// No aliasing of borrows is tracked.
void borrowFlow(FlowState *fstate, RefNode **nodep) {
    borrowFlowPlace(fstate, &(*nodep)->vtexp);
}
