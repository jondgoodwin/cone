/** Region type handling - a region is a struct declaring 'is RegionRef'
 * @file
 *
 * A region's annotation -- the name after '+' -- is an ordinary struct that
 * declares the built-in trait 'RegionRef'. The compiler knows no region by
 * name. What it knows is the methods a region may declare, each found by name
 * and each optional, and it calls them at the reference events it alone can
 * see: 'alloc' and 'init' when '+R value' allocates, 'alias' when a copy of an
 * owning reference becomes another owner, 'dealias' when an owner goes away,
 * and 'free' once the value is dead. What a region leaves out says what it
 * does. compiler/c/doc/nodes/module.md, "What a region is", has the contract.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// The struct a reference's region slot names, or NULL for a borrowed
// reference's region or anything that is not a struct
static StructNode *regionDcl(INode *region) {
    if (region == NULL || region->tag == BorrowRegTag)
        return NULL;
    INode *dcl = itypeGetTypeDcl(region);
    return dcl->tag == StructTag ? (StructNode*)dcl : NULL;
}

// Does this struct declare 'is RegionRef'? Its 'traits' records every
// abstraction its 'is' list named, the first included, once name resolution
// has taken them in.
static int regionStructIsRegionRef(StructNode *strnode) {
    if (strnode->traits == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(strnode->traits, cnt, nodesp)) {
        if (*nodesp == (INode*)regionRefTrait)
            return 1;
    }
    return 0;
}

int regionIsRegionRef(INode *region) {
    StructNode *strnode = regionDcl(region);
    return strnode != NULL && regionStructIsRegionRef(strnode);
}

FnDclNode *regionMethod(INode *region, Name *name) {
    StructNode *strnode = regionDcl(region);
    if (strnode == NULL)
        return NULL;
    INode *meth = iNsTypeFindFnField((INsTypeNode*)strnode, name);
    return meth && meth->tag == FnDclTag ? (FnDclNode*)meth : NULL;
}

int regionIsCounted(INode *region) {
    return regionIsRegionRef(region) && regionMethod(region, aliasMethodName) != NULL;
}

int regionIsOwning(INode *region) {
    return regionIsRegionRef(region);
}

// A region without 'alias' has one owner per value: a copy of a reference to
// it can only be a move. Marked on the struct as '@move' marks it, so the one
// test every reference type asks (refAdoptInfections) answers for both.
void regionNameRes(StructNode *node) {
    if (regionStructIsRegionRef(node) && regionMethod((INode*)node, aliasMethodName) == NULL)
        node->flags |= MoveType;
}

int regionIsPtrU8(RefNode *ptrnode) {
    if (ptrnode->tag != PtrTag)
        return 0;
    NbrNode *nbrnode = (NbrNode*)itypeGetTypeDcl(ptrnode->vtexp);
    if (nbrnode != u8Type)
        return 0;
    return 1;
}

// Is this method's one parameter 'self &uni R', the receiver every region
// method but 'alloc' and 'init' takes: a unique borrow of the region's header?
static int regionIsSelfRef(FnDclNode *meth, StructNode *region) {
    if (!(meth->flags & FlagMethFld))
        return 0;
    FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
    if (sig->parms->used != 1)
        return 0;
    RefNode *selftype = (RefNode*)itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(sig->parms, 0)));
    return selftype->tag == RefTag && selftype->region == borrowRef
        && itypeGetTypeDcl(selftype->perm) == (INode*)uniPerm
        && itypeGetTypeDcl(selftype->vtexp) == (INode*)region;
}

// Check the shape of 'alias', 'dealias' or 'free' where the region declares it:
// '(self &uni R)', returning Bool for 'dealias' and nothing for the others
static void regionCheckSelfMeth(StructNode *region, Name *name, int retbool) {
    INode *member = iNsTypeFindFnField((INsTypeNode*)region, name);
    if (member == NULL)
        return;
    int ok = member->tag == FnDclTag && regionIsSelfRef((FnDclNode*)member, region);
    if (ok) {
        INode *rettype = itypeGetTypeDcl(((FnSigNode*)itypeGetTypeDcl(((FnDclNode*)member)->vtype))->rettype);
        ok = retbool ? rettype == (INode*)boolType : rettype->tag == VoidTag;
    }
    if (ok)
        return;
    if (retbool)
        errorMsgNode(member, ErrorRegionMeth,
            "A region's dealias must be declared 'fn dealias(self &uni %s) Bool': it is handed the header, and answers whether the owner that went was the last.",
            &region->namesym->namestr);
    else
        errorMsgNode(member, ErrorRegionMeth,
            "A region's %s must be declared 'fn %s(self &uni %s)': it is handed the header, and returns nothing.",
            &name->namestr, &name->namestr, &region->namesym->namestr);
}

// Check a static 'alloc' of the shape the compiler calls: one usize, the whole
// allocation's size with the header in it, returning '*u8' (null for failure)
static void regionCheckAlloc(FnDclNode *allocmeth) {
    FnSigNode *allocsig = (FnSigNode*)itypeGetTypeDcl(allocmeth->vtype);
    if (allocsig->parms->used != 1) {
        errorMsgNode((INode*)allocmeth, ErrorBadAlloc, "Region alloc method needs single usize parm.");
        return;
    }
    NbrNode *sizetype = (NbrNode *)itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(allocsig->parms, 0)));
    if (sizetype != usizeType) {
        errorMsgNode((INode*)allocmeth, ErrorBadAlloc, "Region alloc method needs single usize parm.");
        return;
    }
    RefNode *rettype = (RefNode*)itypeGetTypeDcl(allocsig->rettype);
    if (!regionIsPtrU8(rettype))
        errorMsgNode((INode*)allocmeth, ErrorBadAlloc, "Region alloc method must return *u8.");
}

// Check a static 'init' of the shape the compiler calls: no parameters,
// returning the header's initial value, which allocation stores
static void regionCheckInit(FnDclNode *initmeth, StructNode *region) {
    FnSigNode *initsig = (FnSigNode*)itypeGetTypeDcl(initmeth->vtype);
    if (initsig->parms->used != 0) {
        errorMsgNode((INode*)initmeth, ErrorBadAlloc, "Region init method may not have parameters.");
        return;
    }
    INode *initrettype = itypeGetTypeDcl(initsig->rettype);
    if (itypeMatches(initrettype, (INode*)region, Coercion) != EqMatch)
        errorMsgNode((INode*)initmeth, ErrorBadAlloc, "Region init method must return initial value.");
}

// Hold a struct that declares 'is RegionRef' to what that promises: each region
// method it declares has the shape the compiler calls it with. Run once, at the
// declaration, after its methods are type checked, so a malformed region is
// reported whether or not anything allocates from it.
//
// Every method is optional, and an absent one means its operation does not
// happen [Jon 25 Sep]; no combination is refused for what it leaves out:
// - no 'alias': one owner per value ('so'). A copy is a move.
// - no 'dealias': an owner going away reports nothing. With no 'alias' too,
//   every owner's going is the value's death; with 'alias', the value is shared
//   and never dies by count, so it is never freed until something other than
//   its owners frees it -- a collector, when one exists.
// - 'dealias' without 'alias': a single owner whose going still asks 'dealias'.
// - no 'free': the memory is not given back a value at a time.
// - no 'alloc': nothing allocates from the region (refused at '+R value',
//   regionAllocTypeCheck), and an 'init' it has is never called.
// The one refusal is a contradiction: '@move' says one owner, 'alias' another.
// '@move' without 'alias' says again what the absence says, and is accepted.
void regionRefCheck(StructNode *node) {
    StructNode *base = structBaseTraitDcl(node);
    if ((node->flags & (TraitType | EnumType)) || (base != NULL && (base->flags & EnumType))) {
        errorMsgNode((INode*)node, ErrorRegionRefUse,
            "Only a struct may declare 'is RegionRef': a region's annotation is a concrete type whose methods the compiler calls.");
        return;
    }

    INode *allocmember = iNsTypeFindFnField((INsTypeNode*)node, allocMethodName);
    if (allocmember && allocmember->tag != FnDclTag)
        errorMsgNode(allocmember, ErrorBadAlloc, "Region alloc must be a static method: 'fn alloc(size usize) *u8'.");
    else if (allocmember)
        regionCheckAlloc((FnDclNode*)allocmember);

    INode *initmember = iNsTypeFindFnField((INsTypeNode*)node, initMethodName);
    if (initmember && initmember->tag != FnDclTag)
        errorMsgNode(initmember, ErrorBadAlloc, "Region init must be a static method returning the header's initial value.");
    else if (initmember)
        regionCheckInit((FnDclNode*)initmember, node);

    regionCheckSelfMeth(node, aliasMethodName, 0);
    regionCheckSelfMeth(node, dealiasMethodName, 1);
    regionCheckSelfMeth(node, freeMethodName, 0);

    // Judged by the method: a member of the name that is not one was reported
    // above, and is no operation the compiler calls
    if (regionMethod((INode*)node, aliasMethodName) && (node->flags & MoveType))
        errorMsgNode((INode*)node, ErrorRegionSet,
            "Region %s is '@move', one owner per value, but declares alias, which makes another. A region without alias is single owner already.",
            &node->namesym->namestr);
}

// At an allocation '+R value': the region is a RegionRef (refTypeCheck reports
// one that is not) whose 'alloc' the compiler can call
void regionAllocTypeCheck(INode *region) {
    StructNode *strnode = regionDcl(region);
    if (strnode == NULL || !regionStructIsRegionRef(strnode))
        return;
    if (iNsTypeFindFnField((INsTypeNode*)strnode, allocMethodName) == NULL)
        errorMsgNode(region, ErrorBadAlloc, "Region %s cannot allocate: it declares no alloc static method.",
            &strnode->namesym->namestr);
}
