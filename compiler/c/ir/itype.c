/** Generic Type node handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *itypeGetTypeDcl(INode *type) {
    assert(isTypeNode(type));
    // A caller whose slot may hold a name of something other than a type, on
    // a path that has reported it, asks isTypeNode before coming here
    // (refRegionCheck, regionDcl)
    while (1) {
        if (isNameUseNode(type) && isTypeNode(type))
            type = nameUseGetDcl((NameUseNode *)type);
        // A type alias stands for a type expression, and a name use bound to one
        // already answers for its target, so this is the alias reached directly
        else if (type->tag == AliasDclTag)
            type = ((AliasDclNode *)type)->target;
        else
            return type;
    }
}

// Return node's type's declaration node (or vtexp if a ref or ptr)
INode *itypeGetDerefTypeDcl(INode *node) {
    INode *typnode = itypeGetTypeDcl(node);
    if (typnode->tag == RefTag || typnode->tag == VirtRefTag)
        return itypeGetTypeDcl(((RefNode*)typnode)->vtexp);
    else if (typnode->tag == PtrTag)
        return itypeGetTypeDcl(((StarNode*)typnode)->vtexp);
    return typnode;
}

// Set when an answer reached a struct that cannot give a final one: a struct
// still being asked (a cycle through owning references or pointers) or one
// not yet type checked. A "no" that depended on it is not remembered.
static int itypeCarriesProvisional = 0;

static int itypeStructCarriesBorrow(StructNode *type) {
    switch (type->carriesborrow) {
    case CarriesBorrowYes:
        return 1;
    case CarriesBorrowNo:
        return 0;
    case CarriesBorrowAsking:
        // Reached again round a cycle: whatever this struct carries is found
        // where it was first asked, so the loop adds nothing
        itypeCarriesProvisional = 1;
        return 0;
    default:
        break;
    }
    int svprovisional = itypeCarriesProvisional;
    itypeCarriesProvisional = 0;
    type->carriesborrow = CarriesBorrowAsking;

    int carries = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&type->fields, cnt, nodesp)) {
        if (itypeCarriesBorrow(((IExpNode *)*nodesp)->vtype)) {
            carries = 1;
            break;
        }
    }
    // A closed trait or enum holds whichever of its variants the value is.
    // An open trait's implementors are not known here: a value of one is held
    // only through a reference, which answers for itself.
    if (!carries && type->derived) {
        for (nodesFor(type->derived, cnt, nodesp)) {
            if (itypeCarriesBorrow(*nodesp)) {
                carries = 1;
                break;
            }
        }
    }

    // A borrow found is final once the struct is checked. A "no" is final only
    // for a checked struct whose answer did not lean on one still being asked.
    if (!(type->flags & TypeChecked))
        itypeCarriesProvisional = 1;
    if (carries && (type->flags & TypeChecked))
        type->carriesborrow = CarriesBorrowYes;
    else if (!carries && !itypeCarriesProvisional)
        type->carriesborrow = CarriesBorrowNo;
    else
        type->carriesborrow = CarriesBorrowUnknown;
    itypeCarriesProvisional |= svprovisional;
    return carries;
}

// May a value of this type hold a borrowed reference?
int itypeCarriesBorrow(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    // A name, or an alias, answers for the type it stands for. One that names
    // no type -- a generic's parameter in its template -- answers nothing.
    case NameUseTag:
        return isTypeNode(type) ? itypeCarriesBorrow(itypeGetTypeDcl(type)) : 0;
    case AliasDclTag:
        return itypeCarriesBorrow(((AliasDclNode *)type)->target);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        // A borrowed reference to a function is global: a function is never
        // a local, so its borrow can neither dangle nor hold anything frozen.
        // A key is no borrow: its invariant lifetime ends nowhere, and what it
        // names lives in its arena.
        if (itypeGetTypeDcl(((RefNode *)type)->region) == borrowRef)
            return !lifeIsInvariant(((RefNode *)type)->lifename)
                && !(type->tag == RefTag && isTypeNode(((RefNode *)type)->vtexp)
                && itypeGetTypeDcl(((RefNode *)type)->vtexp)->tag == FnSigTag);
        return itypeCarriesBorrow(((RefNode *)type)->vtexp);
    case PtrTag:
        return itypeCarriesBorrow(((StarNode *)type)->vtexp);
    case ArrayTag:
        return itypeCarriesBorrow(arrayElemType(type));
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeCarriesBorrow(*nodesp))
                return 1;
        }
        return 0;
    }
    case StructTag:
        return itypeStructCarriesBorrow((StructNode *)type);
    default:
        return 0;
    }
}

// Can a borrowed reference whose referent is 'want' be stored somewhere in a
// value of type 'type': does it hold, anywhere a store can reach (a field, a
// variant, an element, through an owning reference or a pointer), a borrowed
// reference to that type, or to a trait, which a borrow of any implementer
// becomes? 'want' is a type declaration. Asked only of a call that may store
// through one of its arguments, so the answer is not remembered.
#define HoldsBorrowMaxDepth 16
static INode *holdsBorrowAsking[HoldsBorrowMaxDepth];
static int holdsBorrowDepth = 0;

int itypeHoldsBorrowOf(INode *type, INode *want) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? itypeHoldsBorrowOf(itypeGetTypeDcl(type), want) : 0;
    case AliasDclTag:
        return itypeHoldsBorrowOf(((AliasDclNode *)type)->target, want);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        INode *referent = ((RefNode *)type)->vtexp;
        if (itypeGetTypeDcl(((RefNode *)type)->region) == borrowRef && isTypeNode(referent)) {
            INode *refdcl = itypeGetTypeDcl(referent);
            if (refdcl == want || (refdcl->tag == StructTag && (refdcl->flags & TraitType))
                || (want->tag == ArrayTag && itypeGetTypeDcl(arrayElemType(want)) == refdcl))
                return 1;
        }
        return itypeHoldsBorrowOf(referent, want);
    }
    case PtrTag:
        return itypeHoldsBorrowOf(((StarNode *)type)->vtexp, want);
    case ArrayTag:
        return itypeHoldsBorrowOf(arrayElemType(type), want);
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeHoldsBorrowOf(*nodesp, want))
                return 1;
        }
        return 0;
    }
    case StructTag:
    {
        if (!itypeCarriesBorrow(type))
            return 0;
        for (int i = 0; i < holdsBorrowDepth; ++i) {
            if (holdsBorrowAsking[i] == type)
                return 0;
        }
        if (holdsBorrowDepth == HoldsBorrowMaxDepth)
            return 1;
        holdsBorrowAsking[holdsBorrowDepth++] = type;
        int holds = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&((StructNode *)type)->fields, cnt, nodesp)) {
            if (itypeHoldsBorrowOf(((IExpNode *)*nodesp)->vtype, want)) {
                holds = 1;
                break;
            }
        }
        if (!holds && ((StructNode *)type)->derived) {
            for (nodesFor(((StructNode *)type)->derived, cnt, nodesp)) {
                if (itypeHoldsBorrowOf(*nodesp, want)) {
                    holds = 1;
                    break;
                }
            }
        }
        --holdsBorrowDepth;
        return holds;
    }
    default:
        return 0;
    }
}

// How many writable borrows deep a store into a value of this type can reach,
// at most 'most': 0 when it holds no writable borrowed reference whose
// referent can hold a borrow; else one more than what such a referent reaches
// in turn. A field, a variant, an element, and what an owning reference or a
// pointer owns are the value's own; a read-only borrow is no way through.
// The structs being asked, each with the depth left when it was asked: a type
// reaching itself through what it owns adds nothing round the loop, and one
// reaching itself through a writable borrow is asked again with one less, so
// it ends at 'most'. Asked only of a call that may store through an argument,
// so the answer is not remembered.
#define WritableAskingMax 16
static INode *writableAsking[WritableAskingMax];
static int writableAskingMost[WritableAskingMax];
static int writableAskingCnt = 0;

int itypeWritableBorrowDepth(INode *type, int most) {
    if (type == NULL || most <= 0)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? itypeWritableBorrowDepth(itypeGetTypeDcl(type), most) : 0;
    case AliasDclTag:
        return itypeWritableBorrowDepth(((AliasDclNode *)type)->target, most);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        if (itypeGetTypeDcl(ref->region) != borrowRef)
            return itypeWritableBorrowDepth(ref->vtexp, most);
        if (type->tag == VirtRefTag || !(permGetFlags(ref->perm) & MayWrite)
            || !isTypeNode(ref->vtexp) || !itypeCarriesBorrow(ref->vtexp))
            return 0;
        return 1 + itypeWritableBorrowDepth(ref->vtexp, most - 1);
    }
    case PtrTag:
        return itypeWritableBorrowDepth(((StarNode *)type)->vtexp, most);
    case ArrayTag:
        return itypeWritableBorrowDepth(arrayElemType(type), most);
    case TTupleTag: {
        int depth = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            int d = itypeWritableBorrowDepth(*nodesp, most);
            if (d > depth)
                depth = d;
        }
        return depth;
    }
    case StructTag:
    {
        if (!itypeCarriesBorrow(type))
            return 0;
        for (int i = 0; i < writableAskingCnt; ++i) {
            if (writableAsking[i] == type && writableAskingMost[i] == most)
                return 0;
        }
        if (writableAskingCnt == WritableAskingMax)
            return most;
        writableAsking[writableAskingCnt] = type;
        writableAskingMost[writableAskingCnt++] = most;
        int depth = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&((StructNode *)type)->fields, cnt, nodesp)) {
            int d = itypeWritableBorrowDepth(((IExpNode *)*nodesp)->vtype, most);
            if (d > depth)
                depth = d;
        }
        if (((StructNode *)type)->derived) {
            for (nodesFor(((StructNode *)type)->derived, cnt, nodesp)) {
                int d = itypeWritableBorrowDepth(*nodesp, most);
                if (d > depth)
                    depth = d;
            }
        }
        --writableAskingCnt;
        return depth;
    }
    default:
        return 0;
    }
}

// The structs itypeDropReadsBorrow is asking about. A type reaching itself (a
// node owning the next) adds nothing round the loop: what it reads is found
// where it was first asked. Asked only where a holder dies with a conflict
// pending, which is rare, so the answer is not remembered.
#define DropReadsMaxDepth 16
static StructNode *dropReadsAsking[DropReadsMaxDepth];
static int dropReadsDepth = 0;

static int itypeStructDropReadsBorrow(StructNode *type) {
    for (int i = 0; i < dropReadsDepth; ++i) {
        if (dropReadsAsking[i] == type)
            return 0;
    }
    if (dropReadsDepth == DropReadsMaxDepth)
        return itypeCarriesBorrow((INode *)type);
    dropReadsAsking[dropReadsDepth++] = type;
    // Its own 'final' may read any borrow it holds but through a raw pointer;
    // what a raw pointer reaches -- a collection's elements -- it finalizes
    INode *final = namespaceFind(&type->namespace, finalName);
    int hasfinal = final && final->tag == FnDclTag;
    int reads = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&type->fields, cnt, nodesp)) {
        INode *ftype = ((IExpNode *)*nodesp)->vtype;
        INode *fdcl = ftype && isTypeNode(ftype) ? itypeGetTypeDcl(ftype) : NULL;
        if (fdcl && fdcl->tag == PtrTag)
            reads = itypeDropReadsBorrow(((StarNode *)fdcl)->vtexp);
        else
            reads = hasfinal ? itypeCarriesBorrow(ftype) : itypeDropReadsBorrow(ftype);
        if (reads)
            break;
    }
    // An enum dies as whichever of its variants the value is
    if (!reads && type->derived) {
        for (nodesFor(type->derived, cnt, nodesp)) {
            if (itypeDropReadsBorrow(*nodesp)) {
                reads = 1;
                break;
            }
        }
    }
    --dropReadsDepth;
    return reads;
}

int itypeDropReadsBorrow(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? itypeDropReadsBorrow(itypeGetTypeDcl(type)) : 0;
    case AliasDclTag:
        return itypeDropReadsBorrow(((AliasDclNode *)type)->target);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        // A borrow's death does nothing; an owner's finalizes what it owns
        if (itypeGetTypeDcl(((RefNode *)type)->region) == borrowRef)
            return 0;
        return itypeDropReadsBorrow(((RefNode *)type)->vtexp);
    case ArrayTag:
        return itypeDropReadsBorrow(arrayElemType(type));
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeDropReadsBorrow(*nodesp))
                return 1;
        }
        return 0;
    }
    case StructTag:
        return itypeStructDropReadsBorrow((StructNode *)type);
    default:
        return 0;
    }
}

// ---- The thread check: may a value of this type cross threads? ----------
//
// A type is bound to its thread when it holds, anywhere -- inline, through an
// owning reference, in an array element, a tuple element or an enum's
// variant -- a reference the rule refuses (refThreadBinds: a borrow, a traced
// reference, an owner that may be aliased without a RaceSafe permission or a
// ThreadSafe region), or a raw pointer, whose target nothing checks. Every
// other type is Sendable (genericTypeIs). The walk goes through owning
// references, because an owner that crosses takes what it points at with it:
// a 'uni' owner of a struct holding a 'mut' reference is bound, and so is an
// 'Arc[imm, T]' owner of one. A struct declaring 'Sendable' is taken at its word,
// except that an instance of a generic one is bound where one of its type
// arguments is. An open trait's implementers are not all known, so a
// reference to one is bound.
//
// A type may reach itself through a reference ('next So[Node]'), and a type
// reached through a reference need not be laid out yet, so the walk is the
// same fixed point as itypeCarriesBorrow's: a struct reached again while it is
// being asked adds nothing (what it holds is found where it was first asked),
// and a "not bound" that leaned on such a struct, or on one not yet type
// checked, is not remembered, and is reported as not settled.

static int itypeBoundProvisional = 0;

// Whether a borrow that lives for the whole program is let cross, in the walk
// being made (itypeThreadBoundHow): the walk is the same, but for that one
// reference rule. It answers differently from the answer remembered per
// struct, which is the strict one.
static StaticBorrow itypeStaticHow = StaticOff;

static int itypeThreadBoundAt(INode *type);

// Is one of this instance's type arguments bound to its thread?
static int itypeArgsThreadBound(INode *type) {
    Nodes *args = itypeInstanceTypeArgs(type);
    if (args == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(args, cnt, nodesp)) {
        if (itypeThreadBoundAt(*nodesp))
            return 1;
    }
    return 0;
}

// What a reference says of crossing in the walk being made: a borrow that lives
// for the whole program crosses, if it is imm, opaq or uni and what it points
// at does (refStaticCrosses), where the walk lets it
static RefBinds itypeRefBinds(RefNode *ref) {
    RefBinds binds = refThreadBinds(ref);
    if (binds == RefBindsBorrow && refStaticCrosses(ref, itypeStaticHow) == StaticQualifies)
        return RefCrosses;
    return binds;
}

// A trait whose implementers are open-ended: not an enum or other closed one
static int itypeIsOpenTrait(StructNode *type) {
    return (type->flags & TraitType) && !(type->flags & HasTagField);
}

static int itypeStructThreadBound(StructNode *type) {
    // Letting a borrow of the whole program cross can only free a struct the
    // strict walk binds: a "not bound" remembered holds, a "bound" is asked
    // again and not remembered, nor is a "not bound" this walk finds
    uint8_t strict = type->threadbound;
    switch (strict) {
    case CarriesBorrowYes:
        if (itypeStaticHow == StaticOff)
            return 1;
        break;
    case CarriesBorrowNo:
        return 0;
    case CarriesBorrowAsking:
        itypeBoundProvisional = 1;
        return 0;
    default:
        break;
    }
    int svprovisional = itypeBoundProvisional;
    itypeBoundProvisional = 0;
    type->threadbound = CarriesBorrowAsking;

    int bound = 0;
    if (structDeclaresTrait(type, sendableTrait))
        bound = itypeArgsThreadBound((INode *)type);
    else if (itypeIsOpenTrait(type))
        bound = 1;
    else {
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&type->fields, cnt, nodesp)) {
            if (itypeThreadBoundAt(((IExpNode *)*nodesp)->vtype)) {
                bound = 1;
                break;
            }
        }
        // An enum holds whichever of its variants the value is
        if (!bound && type->derived) {
            for (nodesFor(type->derived, cnt, nodesp)) {
                if (itypeThreadBoundAt(*nodesp)) {
                    bound = 1;
                    break;
                }
            }
        }
    }

    if (!(type->flags & TypeChecked))
        itypeBoundProvisional = 1;
    if (itypeStaticHow != StaticOff)
        type->threadbound = strict;
    else if (bound && (type->flags & TypeChecked))
        type->threadbound = CarriesBorrowYes;
    else if (!bound && !itypeBoundProvisional)
        type->threadbound = CarriesBorrowNo;
    else
        type->threadbound = CarriesBorrowUnknown;
    itypeBoundProvisional |= svprovisional;
    return bound;
}

static int itypeThreadBoundAt(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? itypeThreadBoundAt(itypeGetTypeDcl(type)) : 0;
    case AliasDclTag:
        return itypeThreadBoundAt(((AliasDclNode *)type)->target);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        switch (itypeRefBinds((RefNode *)type)) {
        case RefCrossesAll:
            return 0;
        case RefCrosses:
            return itypeThreadBoundAt(((RefNode *)type)->vtexp);
        default:
            return 1;
        }
    case PtrTag:
        return 1;
    case ArrayTag:
        return itypeThreadBoundAt(arrayElemType(type));
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeThreadBoundAt(*nodesp))
                return 1;
        }
        return 0;
    }
    case StructTag:
        return itypeStructThreadBound((StructNode *)type);
    default:
        return 0;
    }
}

int itypeThreadBoundHow(INode *type, int *settled, StaticBorrow how) {
    int svprovisional = itypeBoundProvisional;
    StaticBorrow svhow = itypeStaticHow;
    itypeBoundProvisional = 0;
    itypeStaticHow = how;
    int bound = itypeThreadBoundAt(type);
    if (settled)
        *settled = bound || !itypeBoundProvisional;
    itypeBoundProvisional = svprovisional;
    itypeStaticHow = svhow;
    return bound;
}

int itypeThreadBound(INode *type, int *settled) {
    return itypeThreadBoundHow(type, settled, StaticOff);
}

// Append to 'buf' a type as the thread check's message spells it: a reference
// or an array as it is written ('&mut Point', 'Rc[imm, Point]', '*u64',
// 'Array[u8, 4]'), a permission always spelled out, anything else by its name
void itypeSpellCat(char *buf, size_t size, INode *type, int depth) {
    size_t used = strlen(buf);
    if (!isTypeNode(type)) {
        snprintf(buf + used, size - used, "?");
        return;
    }
    INode *dcl = itypeGetTypeDcl(type);
    if (depth < 4 && (dcl->tag == RefTag || dcl->tag == ArrayRefTag || dcl->tag == VirtRefTag)) {
        RefNode *ref = (RefNode *)dcl;
        INode *perm = ref->perm && isTypeNode(ref->perm) ? itypeGetTypeDcl(ref->perm) : NULL;
        Name *permname = perm ? inodeGetName(perm) : NULL;
        char *pname = permname ? &permname->namestr : "?";
        INode *region = ref->region && isTypeNode(ref->region) ? itypeGetTypeDcl(ref->region) : ref->region;
        char *shape = dcl->tag == ArrayRefTag ? "[]" : dcl->tag == VirtRefTag ? "<" : "";
        Name *regname = region && region->tag == StructTag ? ((StructNode *)region)->namesym : NULL;
        char *rname = regname ? &regname->namestr : "?";
        if (region == borrowRef) {
            snprintf(buf + used, size - used, "&%s %s", pname, shape);
            itypeSpellCat(buf, size, ref->vtexp, depth + 1);
            return;
        }
        // A managed reference: 'Rc[mut, Point]', whether thin or virtual
        snprintf(buf + used, size - used, "%s[%s, ", rname, pname);
        itypeSpellCat(buf, size, ref->vtexp, depth + 1);
        used = strlen(buf);
        snprintf(buf + used, size - used, "]");
        return;
    }
    if (depth < 4 && dcl->tag == PtrTag) {
        snprintf(buf + used, size - used, "*");
        itypeSpellCat(buf, size, ((StarNode *)dcl)->vtexp, depth + 1);
        return;
    }
    // An array as it is written, 'Array[u8, 4]', a nested one with its sizes
    // together, outermost first: 'Array[f32, 2, 3]'
    if (depth < 4 && dcl->tag == ArrayTag) {
        INode *elem = dcl;
        while (elem->tag == ArrayTag)
            elem = arrayElemType(elem);
        snprintf(buf + used, size - used, "Array[");
        itypeSpellCat(buf, size, elem, depth + 1);
        for (INode *dim = dcl; dim != elem; dim = arrayElemType(dim)) {
            used = strlen(buf);
            snprintf(buf + used, size - used, ", %llu", (unsigned long long)arrayDim1(dim));
        }
        used = strlen(buf);
        snprintf(buf + used, size - used, "]");
        return;
    }
    snprintf(buf + used, size - used, "%s", itypeName(dcl));
}

#define ThreadBoundPathMax 16
#define ThreadBoundPathSize 256

// The culprit walk: the first thread-bound part of 'type', following only
// what itypeThreadBound says is bound, and never into a struct it has passed
// through already, so a cycle whose cause lies off it is left for the cause
static INode *itypeThreadBoundCulprit(INode *type, char *path, size_t size,
        StructNode **seen, uint32_t nseen) {
    if (type == NULL || nseen >= ThreadBoundPathMax)
        return NULL;
    size_t used = strlen(path);
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? itypeThreadBoundCulprit(itypeGetTypeDcl(type), path, size, seen, nseen) : NULL;
    case AliasDclTag:
        return itypeThreadBoundCulprit(((AliasDclNode *)type)->target, path, size, seen, nseen);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        switch (itypeRefBinds((RefNode *)type)) {
        case RefCrossesAll:
            return NULL;
        case RefCrosses:
            // A borrow of the whole program that crosses leaves its target
            // to blame: a struct names itself and its field, anything else
            // is named as what the borrow points at
            if (path[0] == '\0' && refThreadBinds((RefNode *)type) == RefBindsBorrow
                && isTypeNode(((RefNode *)type)->vtexp)
                && itypeGetTypeDcl(((RefNode *)type)->vtexp)->tag != StructTag)
                snprintf(path, size, "what the borrow points at");
            return itypeThreadBoundCulprit(((RefNode *)type)->vtexp, path, size, seen, nseen);
        default:
            return type;
        }
    case PtrTag:
        return type;
    case ArrayTag:
        snprintf(path + used, size - used, used ? "[]" : "an element");
        return itypeThreadBoundCulprit(arrayElemType(type), path, size, seen, nseen);
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        uint32_t index = 0;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeThreadBoundHow(*nodesp, NULL, itypeStaticHow)) {
                snprintf(path + used, size - used, used ? ".%u" : "element %u", index);
                return itypeThreadBoundCulprit(*nodesp, path, size, seen, nseen);
            }
            ++index;
        }
        return NULL;
    }
    case StructTag: {
        StructNode *strnode = (StructNode *)type;
        for (uint32_t i = 0; i < nseen; ++i) {
            if (seen[i] == strnode)
                return NULL;
        }
        seen[nseen++] = strnode;
        if (itypeIsOpenTrait(strnode) && !structDeclaresTrait(strnode, sendableTrait))
            return type;
        INode **nodesp;
        uint32_t cnt;
        // What was said so far, restored where a type argument or a variant,
        // each named afresh, turns out not to hold the culprit
        char saved[ThreadBoundPathSize];
        snprintf(saved, sizeof(saved), "%s", path);
        // Declared Sendable, and bound only by a type argument
        if (structDeclaresTrait(strnode, sendableTrait)) {
            Nodes *args = itypeInstanceTypeArgs(type);
            if (args == NULL)
                return NULL;
            for (nodesFor(args, cnt, nodesp)) {
                if (!itypeThreadBoundHow(*nodesp, NULL, itypeStaticHow))
                    continue;
                path[0] = '\0';
                INode *culprit = itypeThreadBoundCulprit(*nodesp, path, size, seen, nseen);
                if (culprit) {
                    if (path[0] == '\0')
                        snprintf(path, size, "its type argument");
                    return culprit;
                }
                snprintf(path, size, "%s", saved);
            }
            return NULL;
        }
        if (path[0] == '\0') {
            itypeSpellCat(path, size, type, 0);
            used = strlen(path);
        }
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            if (!itypeThreadBoundHow(field->vtype, NULL, itypeStaticHow))
                continue;
            snprintf(path + used, size - used, ".%s", &field->namesym->namestr);
            INode *culprit = itypeThreadBoundCulprit(field->vtype, path, size, seen, nseen);
            if (culprit)
                return culprit;
            path[used] = '\0';
        }
        // An enum's variant is named by itself
        if (strnode->derived) {
            for (nodesFor(strnode->derived, cnt, nodesp)) {
                if (!itypeThreadBoundHow(*nodesp, NULL, itypeStaticHow))
                    continue;
                path[0] = '\0';
                INode *culprit = itypeThreadBoundCulprit(*nodesp, path, size, seen, nseen);
                if (culprit)
                    return culprit;
                snprintf(path, size, "%s", saved);
            }
        }
        return NULL;
    }
    default:
        return NULL;
    }
}

INode *itypeThreadBoundWhyHow(INode *type, char *path, size_t size, StaticBorrow how) {
    StructNode *seen[ThreadBoundPathMax];
    StaticBorrow svhow = itypeStaticHow;
    itypeStaticHow = how;
    path[0] = '\0';
    INode *culprit = itypeThreadBoundCulprit(type, path, size, seen, 0);
    itypeStaticHow = svhow;
    return culprit;
}

INode *itypeThreadBoundWhy(INode *type, char *path, size_t size) {
    return itypeThreadBoundWhyHow(type, path, size, StaticOff);
}

// Set when an answer reached a struct not yet type checked, whose fields may
// not all be known: a "no" that depended on it is not remembered. Shared by
// the walks for a traced reference and for an atomic value, neither of which
// asks the other.
static int itypeTracedProvisional = 0;

// Does a value of this struct hold what 'holdsfn' looks for, in a field or,
// for an enum or a closed trait, in a variant? Remembered in *memo
// (HoldsTraced*) once the struct is type checked.
static int itypeStructHolds(StructNode *type, uint8_t *memo, int (*holdsfn)(INode *)) {
    switch (*memo) {
    case HoldsTracedYes:
        return 1;
    case HoldsTracedNo:
        return 0;
    case HoldsTracedAsking:
        // Only a struct held by value in itself reaches itself again, which
        // layout refuses; what it holds is found where it was first asked
        itypeTracedProvisional = 1;
        return 0;
    default:
        break;
    }
    int svprovisional = itypeTracedProvisional;
    itypeTracedProvisional = 0;
    *memo = HoldsTracedAsking;

    int holds = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&type->fields, cnt, nodesp)) {
        if (holdsfn(((IExpNode *)*nodesp)->vtype)) {
            holds = 1;
            break;
        }
    }
    // An enum or a closed trait holds whichever of its variants the value is
    if (!holds && type->derived) {
        for (nodesFor(type->derived, cnt, nodesp)) {
            if (holdsfn(*nodesp)) {
                holds = 1;
                break;
            }
        }
    }

    if (!(type->flags & TypeChecked))
        itypeTracedProvisional = 1;
    if (holds && (type->flags & TypeChecked))
        *memo = HoldsTracedYes;
    else if (!holds && !itypeTracedProvisional)
        *memo = HoldsTracedNo;
    else
        *memo = HoldsTracedUnknown;
    itypeTracedProvisional |= svprovisional;
    return holds;
}

static int itypeStructHoldsTraced(StructNode *type) {
    return itypeStructHolds(type, &type->holdstraced, itypeHoldsTraced);
}

// Does a value of this type hold an atomic value where it sits: is it a struct
// declaring 'AtomicValue', or a tuple, array, struct or enum holding one
// inline? The walk stops at every reference and pointer: a value holding the
// address of an atomic holds no atomic itself. Asked of a global, whose
// storage may not be read-only when it does, and of a 'const', which may not
// hold one; a struct's answer is remembered once it is checked.
int itypeHoldsAtomic(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? itypeHoldsAtomic(itypeGetTypeDcl(type)) : 0;
    case AliasDclTag:
        return itypeHoldsAtomic(((AliasDclNode *)type)->target);
    case ArrayTag:
        return itypeHoldsAtomic(arrayElemType(type));
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeHoldsAtomic(*nodesp))
                return 1;
        }
        return 0;
    }
    case StructTag: {
        StructNode *strnode = (StructNode *)type;
        if (structDeclaresTrait(strnode, atomicValueTrait))
            return 1;
        return itypeStructHolds(strnode, &strnode->holdsatomic, itypeHoldsAtomic);
    }
    default:
        return 0;
    }
}

// Does a value of this type hold a traced reference where it sits: is it a
// reference into a region declaring 'Traced', or a tuple, array, struct or
// enum holding one inline? The walk stops at every other reference and at
// every pointer: what they point at is not part of the value, and where a
// traced reference may be held behind them is the placement rules' question
// (regionTracedCheckAll). A struct's answer is remembered once it is checked,
// since the trace, the type record, the rules, and later rooting and
// barriers, all ask it.
int itypeHoldsTraced(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? itypeHoldsTraced(itypeGetTypeDcl(type)) : 0;
    case AliasDclTag:
        return itypeHoldsTraced(((AliasDclNode *)type)->target);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        return regionIsTraced(((RefNode *)type)->region);
    case ArrayTag:
        return itypeHoldsTraced(arrayElemType(type));
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeHoldsTraced(*nodesp))
                return 1;
        }
        return 0;
    }
    case StructTag:
        return itypeStructHoldsTraced((StructNode *)type);
    default:
        return 0;
    }
}

// Does a value of this type hold a borrowed reference where it sits, other
// than a reference to a function (which points at code, not at anything that
// dies)? Like itypeHoldsTraced, the walk stops at owning references and
// pointers. Asked only of what a traced region allocates.
int itypeHoldsBorrow(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? itypeHoldsBorrow(itypeGetTypeDcl(type)) : 0;
    case AliasDclTag:
        return itypeHoldsBorrow(((AliasDclNode *)type)->target);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        return itypeGetTypeDcl(((RefNode *)type)->region) == borrowRef
            && itypeGetTypeDcl(((RefNode *)type)->vtexp)->tag != FnSigTag;
    case ArrayTag:
        return itypeHoldsBorrow(arrayElemType(type));
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeHoldsBorrow(*nodesp))
                return 1;
        }
        return 0;
    }
    case StructTag: {
        StructNode *strnode = (StructNode *)type;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            if (itypeHoldsBorrow(((IExpNode *)*nodesp)->vtype))
                return 1;
        }
        if (strnode->derived) {
            for (nodesFor(strnode->derived, cnt, nodesp)) {
                if (itypeHoldsBorrow(*nodesp))
                    return 1;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

// Look for named field/method in type
INode *iTypeFindFnField(INode *type, Name *name) {
    switch (type->tag) {
    case StructTag:
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
        return iNsTypeFindFnField((INsTypeNode*)type, name);
    case PtrTag:
        return iNsTypeFindFnField(ptrType, name);
    default:
        return NULL;
    }
}

// Refuse a name that names a generic type bare -- 'Box' for a 'struct Box[T]' --
// and return 1 if it is one. A generic is a template, not a type: only its
// instances are types, so a name that stops at the generic names no type at all.
// A folded name answers for what it folds; an 'alias' statement does not,
// because its own target is checked, and refused, where the statement is.
int itypeRefuseBareGeneric(INode *type) {
    if (!isNameUseNode(type))
        return 0;
    INode *dcl = ((NameUseNode*)type)->dclnode;
    while (dcl && dcl->tag == AliasDclTag && !(dcl->flags & FlagTypeAlias)) {
        INode *target = ((AliasDclNode*)dcl)->target;
        dcl = (target && isNameUseNode(target)) ? ((NameUseNode*)target)->dclnode : target;
    }
    if (dcl == NULL || dcl->tag != StructTag || ((StructNode*)dcl)->genericinfo == NULL)
        return 0;
    Name *generic = ((StructNode*)dcl)->namesym;
    errorMsgNode(type, ErrorArgCount,
        "%s is generic, so it is not a type: each of its instances is, written with its type arguments as %s[...].",
        &generic->namestr, &generic->namestr);
    // Bound to the error type from here on, so that nothing reached through this
    // name -- an alias's uses, a parameter's arguments -- reports it again
    ((NameUseNode*)type)->dclnode = errorType;
    return 1;
}

// Refuse a name that names a module or a module trait where a type must be, and
// return 1 if it is one. nameUseGroup answers such a name as a type, so
// isTypeNode does not tell it apart; left alone it reaches generation, which has
// no type to give it.
int itypeRefuseModule(INode *type) {
    if (!isNameUseNode(type))
        return 0;
    INode *dcl = nameUseGetDcl((NameUseNode*)type);
    if (dcl == NULL || (dcl->tag != ModuleTag && dcl->tag != ModTraitTag))
        return 0;
    errorMsgNode(type, ErrorNotType, "%s is a %s, not a type.",
        &((NameUseNode*)type)->namesym->namestr,
        dcl->tag == ModuleTag ? "module" : "module trait");
    // Bound to the error type from here on, as a bare generic is
    ((NameUseNode*)type)->dclnode = errorType;
    return 1;
}

// Type check node, expecting it to be a type. Give error and return 0, if not.
int itypeTypeCheck(TypeCheckState *pstate, INode **node) {
    inodeTypeCheckAny(pstate, node);
    // A type that failed as it was checked -- an instance refused where it was
    // asked for -- was reported there, and stands as the error type from here
    // on, which nothing complains about again
    if (inodeIsError(*node)) {
        *node = errorType;
        return 0;
    }
    if (!isTypeNode(*node)) {
        errorMsgNode(*node, ErrorNotTyped, "Expected a type.");
        return 0;
    }
    if (itypeRefuseBareGeneric(*node) || itypeRefuseModule(*node))
        return 0;
    return 1;
}

// Return 1 if nominally (or structurally) identical, 0 otherwise
// Nodes must both be types, but may be name use or declare nodes
int itypeIsSame(INode *node1, INode *node2) {

    node1 = itypeGetTypeDcl(node1);
    node2 = itypeGetTypeDcl(node2);

    // If they are the same type name, types match
    if (node1 == node2)
        return 1;
    if (node1->tag != node2->tag)
        return 0;

    // For non-named types, equality is determined structurally
    // because they specify the same typed parts
    switch (node1->tag) {
    case RefTag: 
        return refIsSame((RefNode*)node1, (RefNode*)node2);
    case VirtRefTag:
        return refIsSame((RefNode*)node1, (RefNode*)node2);
    case ArrayRefTag:
        return arrayRefIsSame((RefNode*)node1, (RefNode*)node2);
    case PtrTag:
        return ptrEqual((StarNode*)node1, (StarNode*)node2);
    case ArrayTag:
        return arrayEqual((ArrayNode*)node1, (ArrayNode*)node2);
    case TTupleTag:
        return ttupleEqual((TupleNode*)node1, (TupleNode*)node2);
    case FnSigTag:
        return fnSigEqual((FnSigNode*)node1, (FnSigNode*)node2);
    case VoidTag:
        return 1;
    default:
        return 0;
    }
}

// Calculate the hash for a type to use in type table indexing
size_t itypeHash(INode *node) {
    INode *type = itypeGetTypeDcl(node);
    switch (type->tag) {
    case RefTag:
    case VirtRefTag:
        return refHash((RefNode*)type);
    case ArrayRefTag:
        return arrayRefHash((RefNode*)type);
    case PermTag:
        // A guard's permission is laid out as its lock is (itypeIsRunSame)
        if (((PermNode*)type)->lock)
            return ((size_t)((PermNode*)type)->lock) >> 3;
        return ((size_t)immPerm) >> 3;  // Hash for all static permissions is the same
    default:
        // Turn type's pointer into the hash, removing expected 0's in bottom bits
        return ((size_t)type) >> 3;
    }
}

// Return 1 if nominally (or structurally) identical at runtime, 0 otherwise
// Nodes must both be types, but may be name use or declare nodes
// Is a companion for indexing into the type table
int itypeIsRunSame(INode *node1, INode *node2) {

    node1 = itypeGetTypeDcl(node1);
    node2 = itypeGetTypeDcl(node2);
    // A guard's permission takes the room its lock does in the allocation's
    // header, and the reference is the same pointer as the lock-managed one
    if (node1->tag == PermTag && ((PermNode*)node1)->lock)
        node1 = (INode*)((PermNode*)node1)->lock;
    if (node2->tag == PermTag && ((PermNode*)node2)->lock)
        node2 = (INode*)((PermNode*)node2)->lock;

    // If they are the same type name, types match
    if (node1 == node2)
        return 1;
    if (node1->tag != node2->tag)
        return 0;

    // For non-named types, equality is determined structurally
    // because they specify the same typed parts
    switch (node1->tag) {
    case RefTag:
        return refIsRunSame((RefNode*)node1, (RefNode*)node2);
    case VirtRefTag:
        return refIsRunSame((RefNode*)node1, (RefNode*)node2);
    case ArrayRefTag:
        return arrayRefIsRunSame((RefNode*)node1, (RefNode*)node2);
    case PtrTag:
        return ptrEqual((StarNode*)node1, (StarNode*)node2);
    case ArrayTag:
        return arrayEqual((ArrayNode*)node1, (ArrayNode*)node2);
    case TTupleTag:
        return ttupleEqual((TupleNode*)node1, (TupleNode*)node2);
    case FnSigTag:
        return fnSigEqual((FnSigNode*)node1, (FnSigNode*)node2);
    case VoidTag:
        return 1;
    case PermTag:
        return 1;    // Static permissions are erased/equivalent at runtime
    default:
        return 0;
    }
}

// Is totype equivalent or a subtype of fromtype
TypeCompare itypeMatches(INode *totype, INode *fromtype, SubtypeConstraint constraint) {
    fromtype = itypeGetTypeDcl(fromtype);
    totype = itypeGetTypeDcl(totype);

    // If they are the same value type info, types match
    if (totype == fromtype)
        return EqMatch;

    // Either side already reported as bad matches anything. The diagnostic that
    // made it bad has been issued, and a mismatch derived from it says nothing
    // the programmer does not already know.
    if (totype == errorType || fromtype == errorType)
        return EqMatch;

    // Type-specific matching logic
    switch (totype->tag) {

    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
        return nbrMatches(totype, fromtype, constraint);

    case StructTag:
        return structMatches((StructNode*)totype, fromtype, constraint);

    case TTupleTag:
        if (fromtype->tag == TTupleTag)
            return itypeIsSame(totype, fromtype) ? EqMatch : NoMatch;
        return NoMatch;

    case ArrayTag:
        if (fromtype->tag == ArrayTag)
            return arrayMatches((ArrayNode*)totype, (ArrayNode*)fromtype, constraint);
        return NoMatch;

    case FnSigTag:
        if (fromtype->tag == FnSigTag)
            return fnSigMatches((FnSigNode*)totype, (FnSigNode*)fromtype, constraint);
        return NoMatch;

    case RefTag:
        if (fromtype->tag == RefTag)
            return refMatches((RefNode*)totype, (RefNode*)fromtype, constraint);
        return NoMatch;

    case VirtRefTag:
        if (fromtype->tag == VirtRefTag)
            return refvirtMatches((RefNode*)totype, (RefNode*)fromtype, constraint);
        else if (fromtype->tag == RefTag)
            return refvirtMatchesRef((RefNode*)totype, (RefNode*)fromtype, constraint);
        return NoMatch;

    case ArrayRefTag:
        if (fromtype->tag == ArrayRefTag)
            return arrayRefMatches((RefNode*)totype, (RefNode*)fromtype, constraint);
        else if (fromtype->tag == RefTag)
            return arrayRefMatchesRef((RefNode*)totype, (RefNode*)fromtype, constraint);
        return NoMatch;

    case PtrTag:
        if (fromtype->tag == RefTag || fromtype->tag == ArrayRefTag)
            return itypeIsSame(((RefNode*)fromtype)->vtexp, ((StarNode*)totype)->vtexp) ? ConvSubtype : NoMatch;
        if (fromtype->tag == PtrTag)
            return ptrMatches((StarNode*)totype, (StarNode*)fromtype, constraint);
        return NoMatch;

    case VoidTag:
        return fromtype->tag == VoidTag ? EqMatch : NoMatch;

    default:
        return itypeIsSame(totype, fromtype) ? EqMatch : NoMatch;
    }
}

// Return a type that is the supertype of both type nodes, or NULL if none found
INode *itypeFindSuper(INode *type1, INode *type2) {
    INode *typ1 = itypeGetTypeDcl(type1);
    INode *typ2 = itypeGetTypeDcl(type2);

    if (typ1->tag != typ2->tag)
        return NULL;
    if (itypeIsSame(typ1, typ2))
        return type1;
    switch (typ1->tag) {
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
        return nbrFindSuper(type1, type2);

    case StructTag:
        return structFindSuper(type1, type2);

    case RefTag:
    case VirtRefTag:
        return refFindSuper(type1, type2);

    default:
        return NULL;
    }
}

// The type arguments a generic instance was instantiated with, or NULL when the
// declaration is not an instance of a generic.
//
// cloneNode stamps the instantiating node on every node of an instance, and for
// a generic instance that node is the call carrying the type arguments. It is
// required to be a call with a non-empty list of types. A macro expansion's node
// is not, and neither is the implementing struct that a trait's default method
// is cloned into: an inherited default is a copy, not an instance.
Nodes *itypeInstanceTypeArgs(INode *dclnode) {
    INode *instnode = dclnode->instnode;
    if (instnode == NULL
        || (instnode->tag != FnCallTag && instnode->tag != TypeLitTag
            && instnode->tag != ArrIndexTag && instnode->tag != FldAccessTag))
        return NULL;
    Nodes *typeargs = ((FnCallNode*)instnode)->args;
    if (typeargs == NULL || typeargs->used == 0)
        return NULL;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(typeargs, cnt, argsp)) {
        if (*argsp == NULL || !isTypeNode(*argsp))
            return NULL;
    }
    return typeargs;
}

// Return true if type has a concrete and instantiable value. 
// Opaque structs, traits, functions will be false.
int itypeIsConcrete(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    return !(dcltype->flags & OpaqueType);
}

// How many hops of an infection path are worth following or printing. Ordinary
// code is one or two; the bound is here so that a pathological nesting -- or a
// by-value cycle already reported and still in the tree -- cannot run this off
// the stack or bury the diagnostic.
#define NoSizeChainMax 8

static INode *itypeNoSizeField(INode *dcltype, uint32_t depth);

// Is this an enum with a variant still being laid out?
//
// An enum's size is the largest of its variants, and generation is what computes
// that. Its own TypeChecked mark says only that its own fields are settled, which
// begins with the tag: its variants are laid out after that, each taking the enum
// as its base -- and when the enum was demanded by its first variant, the rest
// wait until that one is done (structTypeCheck). So the mark cannot be read as
// 'has a size' here, and one variant still in flight is exactly the case where
// the enum has none -- which is what a variant holding its own enum by value asks
// for. No member asks inside that window: members are checked only once every
// layout is done.
//
// In flight, not merely unfinished. A variant not yet begun is not on the demand
// stack, so it cannot close a cycle with the asker, and if it does hold the enum,
// its own field asks again once it is in flight, and is refused.
static INode *itypeVariantPending(INode *dcltype) {
    // TraitType and a derived list are both required: a *variant* carries the
    // closed flags too, inherited from its enum, and has no derived list at all.
    if (dcltype->tag != StructTag || !(dcltype->flags & TraitType)
        || !(dcltype->flags & (HasTagField | SameSize))
        || ((StructNode*)dcltype)->derived == NULL)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(((StructNode*)dcltype)->derived, cnt, nodesp)) {
        if (((*nodesp)->flags & TypeChecking) && !((*nodesp)->flags & TypeChecked))
            return *nodesp;
    }
    return NULL;
}

// The two causes that mean a by-value cycle, which the message names
// (structLayoutCycle) where the layouts in flight show it
static char itypeInFlightCause[] = "is still being laid out, so it would have to contain itself. Break the cycle by holding it through a reference";
static char itypeVariantPendingCause[] = "is an enum with a variant still being laid out, so it would have to contain itself. Break the cycle by holding it through a reference";

// A type's name, for a diagnostic. See itype.h.
char *itypeName(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    switch (dcltype->tag) {
    case FnSigTag:
        return "a function signature";
    case ArrayTag:
        return "an array";
    case RefTag: case VirtRefTag: case ArrayRefTag: case PtrTag:
        return "a reference";
    case TTupleTag:
        return "a tuple";
    default:
        break;
    }
    Name *namesym = inodeGetName(dcltype);
    return namesym ? (char*)&namesym->namestr : "this type";
}

// This type's own reason for having no size, ignoring anything it caught from a
// field, or NULL when it has a size or is unsized only by infection.
//
// Order matters: a trait or an '@unsized' enum, and a struct infected by an
// unsized field, all carry OpaqueType, so each is asked before the plain
// declared-opaque reading that would otherwise absorb it.
static char *itypeNoSizeOwnCause(INode *dcltype, uint32_t depth) {
    // A reference of any kind is one or two pointers wide whatever it points at,
    // so it has a size from the moment it exists, even while its own check is in
    // flight. That happens without any cycle: an alias of '&Quad' written above
    // Quad demands Quad from inside the reference, and a method of Quad taking
    // the alias reaches the same reference again before it finishes.
    if (dcltype->tag == RefTag || dcltype->tag == VirtRefTag
        || dcltype->tag == ArrayRefTag || dcltype->tag == PtrTag)
        return NULL;

    // Still being laid out. Its own fields are what this walk is in the middle
    // of settling, so there is no size to give yet -- and no cycle check is
    // needed to say so, since a finished type would not be in this state.
    if ((dcltype->flags & TypeChecking) && !(dcltype->flags & TypeChecked))
        return itypeInFlightCause;

    // A type whose implementations differ in size has no one size: a trait,
    // whose implementers are open-ended, or an enum that declined the padding.
    // Asked before the pending question below, so that an '@unsized' enum held
    // by value inside one of its variants' layouts is refused because it will
    // never have a size, not because that variant has not finished.
    if (dcltype->tag == StructTag && (dcltype->flags & OpaqueType)
        && (dcltype->flags & TraitType) && !(dcltype->flags & SameSize))
        return (dcltype->flags & EnumType)
            ? "is an '@unsized' enum, so its variants differ in size. Reach it through a reference"
            : "is a trait whose implementations may differ in size. Use a virtual reference, '&<Trait>'";

    // An enum is laid out only once every variant is, whatever its own mark says
    if (itypeVariantPending(dcltype))
        return itypeVariantPendingCause;

    if (!(dcltype->flags & OpaqueType))
        return NULL;

    // A function signature is a description of a call, not a value
    if (dcltype->tag == FnSigTag)
        return "is not a value at all. Use a reference to a function instead";

    if (dcltype->tag == StructTag) {
        // Opacity is infectious. Where a field carried it, this type is not the
        // cause and the caller keeps walking.
        if (itypeNoSizeField(dcltype, depth) != NULL)
            return NULL;

        return "is declared @opaque, so this program is not told how large it is. Hold it through a reference";
    }

    return "has no known size, so it cannot be held by value";
}

// The first field of this struct whose own type has no size, or NULL if none
// does. This is the hop an infected type's size went missing across.
//
// Depth-bounded because the type graph it walks may be cyclic. A cycle through
// a reference is legal and terminates on its own -- a reference is not a struct
// -- but a by-value cycle that was already reported still sits in the tree, and
// this runs on error paths, where the tree is exactly the shape nothing checked.
static INode *itypeNoSizeField(INode *dcltype, uint32_t depth) {
    if (dcltype->tag != StructTag || depth >= NoSizeChainMax)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&((StructNode*)dcltype)->fields, cnt, nodesp)) {
        INode *fldtype = itypeGetTypeDcl(((IExpNode*)*nodesp)->vtype);
        if (itypeNoSizeOwnCause(fldtype, depth + 1) != NULL
            || itypeNoSizeField(fldtype, depth + 1) != NULL)
            return *nodesp;
    }
    return NULL;
}

// Why this type cannot report a size. See itype.h.
char *itypeNoSizeCause(INode *type, INode **rootp) {
    INode *dcltype = itypeGetTypeDcl(type);
    // The types passed through to the cause, for a cycle's message
    INode *passed[NoSizeChainMax];
    uint32_t hops = 0;
    while (++hops <= NoSizeChainMax) {
        char *own = itypeNoSizeOwnCause(dcltype, 0);
        if (own) {
            if (rootp)
                *rootp = dcltype;
            // Only by-value containment lays a type out, so a size asked of
            // one still in flight closes a by-value cycle, named type by type
            // from the layouts in flight: "A contains B contains A by value"
            char *cycle = NULL;
            if (own == itypeInFlightCause)
                cycle = structLayoutCycle(dcltype, dcltype, passed, hops - 1);
            else if (own == itypeVariantPendingCause)
                cycle = structLayoutCycle(dcltype, itypeVariantPending(dcltype), passed, hops - 1);
            return cycle ? cycle : own;
        }
        INode *fld = itypeNoSizeField(dcltype, 0);
        if (fld == NULL)
            return NULL;
        passed[hops - 1] = dcltype;
        dcltype = itypeGetTypeDcl(((IExpNode*)fld)->vtype);
    }
    if (rootp)
        *rootp = dcltype;
    return "has no known size, and the chain that led there is deeper than this message will follow";
}

// Name the path from an unsized type to its cause. See itype.h.
void itypeNoSizeExplain(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    uint32_t hops = 0;
    while (++hops <= NoSizeChainMax) {
        // Its own cause is what the diagnostic already said, so the walk ends
        // rather than repeating it
        if (itypeNoSizeOwnCause(dcltype, 0) != NULL)
            return;
        INode *fld = itypeNoSizeField(dcltype, 0);
        if (fld == NULL)
            return;
        INode *fldtype = itypeGetTypeDcl(((IExpNode*)fld)->vtype);
        errorMsgNode(fld, Uncounted, "... %s has no size because its field %s has type %s",
            itypeName(dcltype), &inodeGetName(fld)->namestr, itypeName(fldtype));
        dcltype = fldtype;
    }
}

// Return true if type has zero size (e.g., void, empty struct)
int itypeIsZeroSize(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    return dcltype->flags & ZeroSizeType;
}

// Is this 'Never', the return type of a function that does not return? Asked
// of a declared type before it is type checked too, so a name use is followed
// only where name resolution bound it
int itypeIsNever(INode *type) {
    while (type && isNameUseNode(type) && ((NameUseNode *)type)->dclnode)
        type = ((NameUseNode *)type)->dclnode;
    return type != NULL && type == neverType;
}

// Return true if type implements move semantics
int itypeIsMove(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    // A tuple is like a struct: it moves when any of its elements does. It has
    // no declaration to carry the flag, so the question is asked of its elements.
    if (dcltype->tag == TTupleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)dcltype)->elems, cnt, nodesp)) {
            if (itypeIsMove(*nodesp))
                return 1;
        }
        return 0;
    }
    return dcltype->flags & MoveType;
}

// Return true if this is an instantiation of a generic type, such as 'Box[i64]'.
//
// An instantiation is an unlowered FnCallNode until type check replaces it with
// the instance it names, and isTypeNode asks this so that the passes running
// before then -- name resolution's type-versus-value disambiguation, above all
// -- can tell one from a call. Without it '*Box[i64]' reads as a dereference,
// so an instantiation is a type everywhere but inside a composite type.
//
// What tells a generic from anything else is the GenericInfo its declaration
// carries: a generic is an ordinary FnDcl or StructNode with a type parameter
// list attached, which is also how genericSubstitute recognizes one. Only a
// struct's instantiation is a type: a generic function's names a function, and
// a macro's names a MacroDcl.
int itypeIsGenericType(INode *type) {
    if (type->tag != FnCallTag)
        return 0;
    FnCallNode *gentype = (FnCallNode*)type;
    // 'new Pair[i32, f32](1, 2.)' is a value: its type is its objfn
    if (!isNameUseNode(gentype->objfn) || (gentype->flags & FlagNew))
        return 0;
    INode *dclnode = nameUseGetDcl((NameUseNode*)gentype->objfn);
    if (dclnode == NULL || dclnode->tag != StructTag || genericGetInfo(dclnode) == NULL)
        return 0;
    return gentype->args != NULL && gentype->args->used > 0 && nodesGet(gentype->args, 0) != NULL;
}

// The region a managed reference type names at its head, 'Rc' in
// 'Rc[mut, Node]' or the generic 'R' in 'R[A][mut, Node]', as a struct whose
// 'is' list names RegionRef; else NULL. Only the head is asked about.
INode *itypeManagedRefRegion(INode *type) {
    FnCallNode *call = (FnCallNode*)type;
    if (call->tag != FnCallTag || !(call->flags & FlagIndex) || call->methfld != NULL)
        return NULL;
    INode *head = call->objfn;
    // A generic region, 'R[A]', resolves first, then the reference's brackets:
    // its first bracket group is its own type arguments, never a reference
    int instance = head->tag == FnCallTag && itypeIsGenericType(head);
    if (instance)
        head = ((FnCallNode*)head)->objfn;
    if (!isNameUseNode(head))
        return NULL;
    INode *dcl = nameUseGetDcl((NameUseNode*)head);
    if (dcl == NULL || dcl->tag != StructTag || !regionStructWritesRegionRef((StructNode*)dcl)
        || (genericGetInfo(dcl) != NULL) != instance)
        return NULL;
    return dcl;
}

// Is this a managed reference type, 'Rc[Node]' or 'Rc[mut, Node]'? It is a call
// node until type check lowers it into the reference node it names
// (fnCallLowerManagedRef), and is a type all the while, as a generic's
// instantiation is. A region's head with values in its brackets is no type
// but the region's value as a struct's literal, 'Rc[1usize]', which type check
// refuses for 'new Rc(1usize)' (ErrorStructBracket). So
// every argument must be a type -- a permission is one -- or, in a generic's
// template, a type parameter still to be substituted.
int itypeIsManagedRefType(INode *type) {
    if (type->tag != FnCallTag)
        return 0;
    FnCallNode *call = (FnCallNode*)type;
    if (call->args == NULL || call->args->used == 0 || itypeManagedRefRegion(type) == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(call->args, cnt, nodesp)) {
        if (*nodesp == NULL || !(isTypeNode(*nodesp) || inodeIsProvisionalType(*nodesp)))
            return 0;
    }
    return 1;
}

// Return drop function (or NULL) for type
INode *itypeGetDropFnDcl(INode *typenode) {
    INode *type = itypeGetTypeDcl(typenode);
    switch (type->tag) {
    case StructTag:
        return ((StructNode*)type)->dropfn;
    default:
        return NULL;
    }
}

// Whether a value of this type has anything to do when it dies in place: it is
// an owning reference whose release does something (its region's 'dealiasRef',
// or a 'Move' region's death), or its type has a drop function (a struct's or
// an enum's: its 'final', then each field's death, then its owners' release),
// or it is a tuple or an array holding any such value. This is what the
// 'finalize' intrinsic does (genlFinalizeAt), what a scope's end does to a
// local, and what 'needsFinal' answers, in Cone's terms. An owner into a
// region ref with neither -- a traced region's, whose values its collector
// frees -- has nothing to do.
//
// A struct or an enum is asked only for its drop: a type held by value is laid
// out, its drop settled, before any holder's is (structSetDropFn), and one held
// in a cycle was refused and has none.
int itypeNeedsFinal(INode *typenode) {
    if (itypeGetDropFnDcl(typenode) != NULL)
        return 1;
    INode *type = itypeGetTypeDcl(typenode);
    INode **nodesp;
    uint32_t cnt;
    switch (type->tag) {
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        return regionReleaseActs(((RefNode *)type)->region);
    case TTupleTag:
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (itypeNeedsFinal(*nodesp))
                return 1;
        }
        return 0;
    case ArrayTag:
        return itypeNeedsFinal(arrayElemType(type));
    default:
        return 0;
    }
}
