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
 * does, and whether it declares 'Move' says whether a copy is a move and an
 * owner's going a death. A region that also declares 'Traced' has its
 * references found by tracing: each type's record carries a trace that hands
 * every such reference a value holds to the region's 'mark', and where such a
 * reference may be held is restricted to where a collector can find it.
 * compiler/c/doc/nodes/module.md, "What a region is", has the contract.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>

// The struct a reference's region slot names, or NULL for a borrowed
// reference's region or anything that is not a struct
static StructNode *regionDcl(INode *region) {
    if (region == NULL || region->tag == BorrowRegTag)
        return NULL;
    INode *dcl = itypeGetTypeDcl(region);
    return dcl->tag == StructTag ? (StructNode*)dcl : NULL;
}

// Does this struct declare 'is RegionRef'?
static int regionStructIsRegionRef(StructNode *strnode) {
    return structDeclaresTrait(strnode, regionRefTrait);
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

// The region ref's own move-ness: 'is Move' marks the struct MoveType, which is
// what every reference type into it asks (refAdoptInfections)
int regionIsMove(INode *region) {
    return regionIsRegionRef(region) && itypeIsMove(region);
}

int regionIsOwning(INode *region) {
    return regionIsRegionRef(region);
}

// Whether any struct of this compile declared 'is RegionRef, Traced', set as
// each region is checked (regionRefCheck). Until one has, no reference is
// traced, and the placement rules have nothing to look for.
static int regionTracedDeclared = 0;

int regionIsTraced(INode *region) {
    StructNode *strnode = regionDcl(region);
    return strnode != NULL && structDeclaresTrait(strnode, tracedTrait) && regionStructIsRegionRef(strnode);
}

int regionIsPtrU8(RefNode *ptrnode) {
    if (ptrnode->tag != PtrTag)
        return 0;
    NbrNode *nbrnode = (NbrNode*)itypeGetTypeDcl(ptrnode->vtexp);
    if (nbrnode != u8Type)
        return 0;
    return 1;
}

// Does this method take 'nparms' parameters, the first 'self &uni R', the
// receiver every region method but 'alloc' and 'init' takes: a unique borrow
// of the region's header?
static int regionIsSelfRef(FnDclNode *meth, StructNode *region, uint32_t nparms) {
    if (!(meth->flags & FlagMethFld))
        return 0;
    FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
    if (sig->parms->used != nparms)
        return 0;
    RefNode *selftype = (RefNode*)itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(sig->parms, 0)));
    return selftype->tag == RefTag && selftype->region == borrowRef
        && itypeGetTypeDcl(selftype->perm) == (INode*)uniPerm
        && itypeGetTypeDcl(selftype->vtexp) == (INode*)region;
}

// Does this method return nothing?
static int regionMethReturnsNothing(FnDclNode *meth) {
    return itypeGetTypeDcl(((FnSigNode*)itypeGetTypeDcl(meth->vtype))->rettype)->tag == VoidTag;
}

// Check the shape of 'alias', 'dealias' or 'free' where the region declares it:
// '(self &uni R)', returning Bool for 'dealias' and nothing for the others
static void regionCheckSelfMeth(StructNode *region, Name *name, int retbool) {
    INode *member = iNsTypeFindFnField((INsTypeNode*)region, name);
    if (member == NULL)
        return;
    int ok = member->tag == FnDclTag && regionIsSelfRef((FnDclNode*)member, region, 1);
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

// Is 'mark' of one of the two shapes a trace calls: '(self &uni R)', or
// '(self &uni R, perm u32, mode u32)', returning nothing? The longer shape is
// how a region asks to be told, with each reference, its permission and the
// mode its trace was called in (regionMarkTakesContext).
static int regionMarkShapeOk(FnDclNode *meth, StructNode *region) {
    if (!regionMethReturnsNothing(meth))
        return 0;
    if (regionIsSelfRef(meth, region, 1))
        return 1;
    if (!regionIsSelfRef(meth, region, 3))
        return 0;
    FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
    return itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(sig->parms, 1))) == (INode*)u32Type
        && itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(sig->parms, 2))) == (INode*)u32Type;
}

// A region declaring 'Traced' promises a 'mark' for its trace to call, and a
// collector for values it frees one owner at a time would be a contradiction:
// 'Move' says each value's one owner decides its death.
static void regionCheckTraced(StructNode *region) {
    regionTracedDeclared = 1;
    char *name = &region->namesym->namestr;
    if (region->flags & MoveType)
        errorMsgNode((INode*)region, ErrorRegionSet,
            "Region %s is Move, one owner per value whose going is the value's death, but declares Traced, a collector finding what is still reached. A traced region is not Move.",
            name);
    INode *member = iNsTypeFindFnField((INsTypeNode*)region, markMethodName);
    if (member == NULL) {
        errorMsgNode((INode*)region, ErrorTracedMark,
            "Region %s declares Traced, so its trace hands it each reference it finds, through a method it has not declared: 'fn mark(self &uni %s)', or 'fn mark(self &uni %s, perm u32, mode u32)'.",
            name, name, name);
        return;
    }
    if (member->tag != FnDclTag || !regionMarkShapeOk((FnDclNode*)member, region))
        errorMsgNode(member, ErrorRegionMeth,
            "A traced region's mark must be declared 'fn mark(self &uni %s)', or 'fn mark(self &uni %s, perm u32, mode u32)' to be told each reference's permission and the trace's mode: it is handed the header, and returns nothing.",
            name, name);
}

// Check a static 'alloc' of a shape the compiler calls: a usize, the whole
// allocation's size with the header in it, and optionally the value type's
// record ('ty *TypeRecord'), returning '*u8' (null for failure). Which shape it
// has is how a region asks for the record: the compiler passes one only to an
// 'alloc' that takes it (regionAllocTakesRecord).
static void regionCheckAlloc(FnDclNode *allocmeth) {
    FnSigNode *allocsig = (FnSigNode*)itypeGetTypeDcl(allocmeth->vtype);
    uint32_t nparms = allocsig->parms->used;
    NbrNode *sizetype = nparms >= 1 ? (NbrNode *)itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(allocsig->parms, 0))) : NULL;
    if (nparms < 1 || nparms > 2 || sizetype != usizeType
        || (nparms == 2 && !typeRecordIsPtr(((VarDclNode *)nodesGet(allocsig->parms, 1))->vtype))) {
        errorMsgNode((INode*)allocmeth, ErrorBadAlloc,
            "Region alloc method takes 'size usize', and may take the value's type record after it: 'fn alloc(size usize, ty *TypeRecord) *u8'.");
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
// happen [Jon 25 Sep, 26 Sep]; no combination is refused for what it leaves
// out. One owner per value is said explicitly, by 'is Move' [Jon 26 Sep]:
// - no 'alias': a copy of a reference calls nothing. It is a move where the
//   region is 'Move' ('so'), and free where it is not (a collector's shape).
// - no 'dealias': an owner going away asks nothing. Where the region is
//   'Move', every owner's going is the value's death. Where it is not, the
//   going does nothing: the value never dies by an owner, and the compiler
//   never frees it -- the region owns death, in its own loop.
// - 'dealias': every owner's going asks it, and its true is the death.
// - no 'free': the memory is not given back a value at a time.
// - no 'alloc': nothing allocates from the region (refused at '+R value',
//   regionAllocTypeCheck), and an 'init' it has is never called.
// The refusals are contradictions: 'Move' says one owner, 'alias' another,
// and 'Traced' a collector; and 'Traced' without the 'mark' it promises.
void regionRefCheck(StructNode *node) {
    StructNode *base = structBaseTraitDcl(node);
    if ((node->flags & (TraitType | EnumType)) || (base != NULL && (base->flags & EnumType))) {
        errorMsgNode((INode*)node, ErrorRegionRefUse,
            "Only a struct may declare 'is RegionRef': a region's annotation is a concrete type whose methods the compiler calls.");
        return;
    }

    INode *allocmember = iNsTypeFindFnField((INsTypeNode*)node, allocMethodName);
    if (allocmember && allocmember->tag != FnDclTag)
        errorMsgNode(allocmember, ErrorBadAlloc, "Region alloc must be a static method: 'fn alloc(size usize) *u8', or 'fn alloc(size usize, ty *TypeRecord) *u8'.");
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
            "Region %s is Move, one owner per value, but declares alias, which makes another. A counted region is not Move; a single-owner one declares no alias.",
            &node->namesym->namestr);

    if (structDeclaresTrait(node, tracedTrait))
        regionCheckTraced(node);
}

// 'Traced' says something only of a region ref
void regionTracedUseCheck(StructNode *node) {
    if (structDeclaresTrait(node, tracedTrait) && !regionIsRegionRef((INode*)node))
        errorMsgNode((INode*)node, ErrorTracedUse,
            "Only a region ref may declare Traced, a struct declaring 'is RegionRef, Traced': it says the region's references are traced, and %s is no region ref.",
            &node->namesym->namestr);
}

// Does the region's 'alloc' take the value type's record as well as the size?
// Only a region whose 'alloc' asks is handed one, so a region that does not
// ask (so, rc) is called exactly as it would be if records did not exist
int regionAllocTakesRecord(INode *region) {
    FnDclNode *allocmeth = regionMethod(region, allocMethodName);
    if (allocmeth == NULL)
        return 0;
    FnSigNode *allocsig = (FnSigNode*)itypeGetTypeDcl(allocmeth->vtype);
    return allocsig->parms->used == 2;
}

// Does the traced region's 'mark' take each reference's permission and the
// trace's mode after the header?
int regionMarkTakesContext(INode *region) {
    FnDclNode *markmeth = regionMethod(region, markMethodName);
    if (markmeth == NULL)
        return 0;
    FnSigNode *marksig = (FnSigNode*)itypeGetTypeDcl(markmeth->vtype);
    return marksig->parms->used == 3;
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

// ---- Where a traced reference may be held ------------------------------------
//
// A traced reference may be held only where a collector can find it: a local,
// a parameter, a temporary, a value inline in one of those, or a value a traced
// region allocated. The places it may not go are each refused where they are
// written: an owning reference of a region that is not traced to a value
// holding one (rule 1), a global (rule 2), and raw memory placed by
// mem.writeRaw or mem.moveRaw -- an arena's, a pool's, a collection's block
// (rule 4). What a traced region allocates may not hold a borrow (rule 3), must
// be reached by a single reference (rule 5), and may sit behind no permission
// that takes room (rule 6).
//
// Whether a type holds a traced reference is known only once it and every type
// it holds inline are laid out, which a reference type met inside the struct it
// points at ('next +rc Node') is not. So each place a rule looks at is noted as
// type check meets it, and all are judged once type check has finished
// (regionTracedCheckAll), when every answer is final. A compile declaring no
// traced region judges none.

typedef enum {
    TracedAtRef,        // An owning reference type: rules 1, 3, 5 and 6
    TracedAtGlobal,     // A global or static: rule 2
    TracedAtRaw         // An instance of mem.writeRaw or mem.moveRaw: rule 4
} TracedAt;

typedef struct {
    INode *node;        // What was written: the reference type, the variable, the intrinsic
    INode *where;       // Where to report it
    INode *type;        // The type asked about: the value, the variable's, the type argument
    TracedAt at;
} TracedPlace;

static TracedPlace *regionTracedPlaces = NULL;
static uint32_t regionTracedCnt = 0;
static uint32_t regionTracedMax = 0;

static void regionTracedNote(TracedAt at, INode *node, INode *where, INode *type) {
    if (regionTracedCnt == regionTracedMax) {
        uint32_t newmax = regionTracedMax ? regionTracedMax * 2 : 64;
        TracedPlace *places = (TracedPlace *)memAllocBlk(newmax * sizeof(TracedPlace));
        if (regionTracedCnt)
            memcpy(places, regionTracedPlaces, regionTracedCnt * sizeof(TracedPlace));
        regionTracedPlaces = places;
        regionTracedMax = newmax;
    }
    TracedPlace *place = &regionTracedPlaces[regionTracedCnt++];
    place->node = node;
    place->where = where;
    place->type = type;
    place->at = at;
}

// A reference type is checked: an owning one is noted for the rules. Its
// region, permission and pointee are type checked already.
void regionTracedRefNote(RefNode *node) {
    if (regionIsRegionRef(node->region))
        regionTracedNote(TracedAtRef, (INode*)node, (INode*)node, node->vtexp);
}

// A global or a static is checked: noted for rule 2
void regionTracedGlobalNote(VarDclNode *var) {
    regionTracedNote(TracedAtGlobal, (INode*)var, (INode*)var, var->vtype);
}

// An instance of an intrinsic is checked: mem.writeRaw's and mem.moveRaw's
// are noted for rule 4, reported where the program asked for them -- through a
// collection's generic body, at the collection's instance in the program's own
// source, the outermost place that asked, since that is where a traced type
// argument was chosen
void regionTracedRawNote(FnDclNode *fndcl, int16_t intrinsic, INode *typearg) {
    if (intrinsic != WriteRawIntrinsic && intrinsic != MoveRawIntrinsic)
        return;
    INode *where = fndcl->instnode ? fndcl->instnode : (INode *)fndcl;
    while (where->instnode != NULL && where->instnode != where)
        where = where->instnode;
    regionTracedNote(TracedAtRaw, (INode*)fndcl, where, typearg);
}

// A type as a diagnostic names it: itypeName, except that an owning reference
// is spelled out, '+R-perm T', since the type argument a rule complains of is
// so often one. Good until the next call.
static char *regionTracedTypeName(INode *type) {
    static char buf[256];
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag != RefTag || regionDcl(((RefNode *)dcl)->region) == NULL)
        return itypeName(type);
    RefNode *ref = (RefNode *)dcl;
    Name *permname = inodeGetName(itypeGetTypeDcl(ref->perm));
    snprintf(buf, sizeof(buf), "+%s-%s %s", &regionDcl(ref->region)->namesym->namestr,
        permname ? &permname->namestr : "?", itypeName(ref->vtexp));
    return buf;
}

// Rules 1, 3, 5 and 6, on an owning reference type
static void regionTracedJudgeRef(RefNode *node, INode *where) {
    StructNode *region = regionDcl(node->region);
    char *regname = &region->namesym->namestr;
    if (!regionIsTraced(node->region)) {
        if (itypeHoldsTraced(node->vtexp))
            errorMsgNode(where, ErrorTracedHeld,
                "A +%s reference may not point at %s, which holds a traced reference: %s is not traced, so no collector would find what its values hold. A traced reference may be held in a local, a parameter, or a value a traced region allocates.",
                regname, regionTracedTypeName(node->vtexp), regname);
        return;
    }
    if (node->tag != RefTag) {
        errorMsgNode(where, ErrorTracedRefKind,
            "%s is traced, and its trace follows a single reference '+%s T' only: an owning slice's length, and the value type behind a virtual reference, are not where it could find them.",
            regname, regname);
        return;
    }
    INode *perm = itypeGetTypeDcl(node->perm);
    if (perm->tag != PermTag && !itypeIsZeroSize(perm)) {
        errorMsgNode(where, ErrorTracedPerm,
            "%s is traced, and its collector finds each value right after the region's header, so the reference's permission may take no room: %s does.",
            regname, itypeName(perm));
        return;
    }
    if (itypeHoldsBorrow(node->vtexp))
        errorMsgNode(where, ErrorTracedBorrow,
            "A +%s reference may not point at %s, which holds a borrowed reference: a traced value lives as long as it is reached, past any lifetime a borrow is checked against, and its trace cannot see a borrow.",
            regname, regionTracedTypeName(node->vtexp));
}

// Rule 4, on an instance of mem.writeRaw or mem.moveRaw. One asked for
// through a generic's body -- a collection's -- is reported at the program's
// own instantiation, once however many raw moves that body makes.
static void regionTracedJudgeRaw(TracedPlace *place, uint32_t index) {
    FnDclNode *fndcl = (FnDclNode *)place->node;
    if (!itypeHoldsTraced(place->type))
        return;
    for (uint32_t i = 0; i < index; ++i) {
        TracedPlace *prior = &regionTracedPlaces[i];
        if (prior->at == TracedAtRaw && prior->where == place->where && prior->node != place->node
            && itypeHoldsTraced(prior->type))
            return;
    }
    if (place->where != fndcl->instnode)
        errorMsgNode(place->where, ErrorTracedRaw,
            "What this instantiates would place %s, which holds a traced reference, in raw memory no collector traces (by mem.%s, as an arena, a pool or a collection places its values). A traced reference may be held in a local, a parameter, or a value a traced region allocates.",
            regionTracedTypeName(place->type), &fndcl->namesym->namestr);
    else
        errorMsgNode(place->where, ErrorTracedRaw,
            "mem.%s would place %s, which holds a traced reference, in raw memory no collector traces (an arena's, a pool's, a collection's block). A traced reference may be held in a local, a parameter, or a value a traced region allocates.",
            &fndcl->namesym->namestr, regionTracedTypeName(place->type));
}

// Judge every place noted while type check ran, now that every type is laid
// out and each answer is final. Called once, when type check has finished.
void regionTracedCheckAll() {
    uint32_t cnt = regionTracedCnt;
    regionTracedCnt = 0;
    if (!regionTracedDeclared)
        return;
    for (uint32_t i = 0; i < cnt; ++i) {
        TracedPlace *place = &regionTracedPlaces[i];
        if (place->type == NULL || place->type == errorType || place->type == unknownType)
            continue;
        switch (place->at) {
        case TracedAtRef:
            regionTracedJudgeRef((RefNode *)place->node, place->where);
            break;
        case TracedAtGlobal:
            if (itypeHoldsTraced(place->type))
                errorMsgNode(place->where, ErrorTracedGlobal,
                    "%s is a global, and its type %s holds a traced reference, which no collector finds in a global. A traced reference may be held in a local, a parameter, or a value a traced region allocates.",
                    &((VarDclNode *)place->node)->namesym->namestr, regionTracedTypeName(place->type));
            break;
        case TracedAtRaw:
            regionTracedJudgeRaw(place, i);
            break;
        }
    }
}
