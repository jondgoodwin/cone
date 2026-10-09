/** Named lifetimes: on a function's signature, on a struct, and ordered
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

// *********************
// Orders
// *********************

LifeOrder *newLifeOrder() {
    LifeOrder *order = memAllocBlk(sizeof(LifeOrder));
    order->pairs = NULL;
    order->at = NULL;
    order->count = 0;
    order->cap = 0;
    return order;
}

void lifeOrderAdd(LifeOrder *order, Name *longer, Name *shorter, INode *at) {
    if (order->count == order->cap) {
        uint32_t cap = order->cap ? order->cap << 1 : 4;
        Name **pairs = memAllocBlk(2 * cap * sizeof(Name *));
        INode **ats = memAllocBlk(cap * sizeof(INode *));
        if (order->count) {
            memcpy(pairs, order->pairs, 2 * order->count * sizeof(Name *));
            memcpy(ats, order->at, order->count * sizeof(INode *));
        }
        order->pairs = pairs;
        order->at = ats;
        order->cap = cap;
    }
    order->pairs[2 * order->count] = longer;
    order->pairs[2 * order->count + 1] = shorter;
    order->at[order->count++] = at;
}

LifeParms *newLifeParms() {
    LifeParms *parms = memAllocBlk(sizeof(LifeParms));
    parms->names = NULL;
    parms->contains = NULL;
    parms->order = NULL;
    parms->usedat = NULL;
    parms->count = 0;
    parms->inferred = 0;
    return parms;
}

int lifeParmsDeclare(LifeParms *parms, Name *name, INode *at) {
    for (uint32_t k = 0; k < parms->count; ++k) {
        if (parms->names[k] == name) {
            errorMsgNode(at, ErrorDupName, "'%s' is declared already.", &name->namestr);
            return 0;
        }
    }
    if (name == staticLifeName) {
        errorMsgNode(at, ErrorLifetimePlace, "''static' is the global lifetime: a struct declares lifetimes of its own.");
        return 0;
    }
    if (parms->count == LifeMaxSlots) {
        errorMsgNode(at, ErrorLifetimeArgs, "A struct declares at most %u lifetimes.", (unsigned)LifeMaxSlots);
        return 0;
    }
    Name **names = memAllocBlk((parms->count + 1) * sizeof(Name *));
    if (parms->count)
        memcpy(names, parms->names, parms->count * sizeof(Name *));
    names[parms->count++] = name;
    parms->names = names;
    return 1;
}

// An invariant lifetime is not erased: a key stays a key in the instance, and
// which brands are one and which apart is part of what it is instanced at.
// lifeCanonBrands then names them by their order, so that one instance serves
// every use whose brands fall alike.
static Name *lifeErasedName(Name *name) {
    return lifeIsInvariant(name) ? name : NULL;
}

INode *lifeErased(INode *type) {
    if (type == NULL)
        return type;
    switch (type->tag) {
    case NameUseTag:
    {
        LifeUse *lifeuse = ((NameUseNode *)type)->lifeuse;
        if (lifeuse == NULL)
            return type;
        NameUseNode *copy = memAllocBlk(sizeof(NameUseNode));
        memcpy(copy, type, sizeof(NameUseNode));
        copy->lifeuse = NULL;
        int anyinv = 0;
        for (uint32_t k = 0; lifeuse->names && k < lifeuse->count; ++k)
            anyinv |= lifeIsInvariant(lifeuse->names[k]);
        if (anyinv) {
            LifeUse *erased = memAllocBlk(sizeof(LifeUse));
            erased->names = memAllocBlk(lifeuse->count * sizeof(Name *));
            for (uint32_t k = 0; k < lifeuse->count; ++k)
                erased->names[k] = lifeErasedName(lifeuse->names[k]);
            erased->count = lifeuse->count;
            erased->typeargs = NULL;
            erased->at = lifeuse->at;
            erased->held = NULL;
            copy->lifeuse = erased;
        }
        return (INode *)copy;
    }
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        INode *vtexp = lifeErased(ref->vtexp);
        if (lifeErasedName(ref->lifename) == ref->lifename && vtexp == ref->vtexp && ref->bound == NULL
            && ref->scope == 0)
            return type;
        RefNode *copy = memAllocBlk(sizeof(RefNode));
        memcpy(copy, ref, sizeof(RefNode));
        copy->lifename = lifeErasedName(ref->lifename);
        copy->bound = NULL;
        copy->vtexp = vtexp;
        // An argument inferred from a borrow carries the caller's band, which
        // is no lifetime of the instance's: as a written type argument has
        // none, it has none. What the instance's body does with a T is the
        // loan walk's, which holds what a parameter lends as the caller's.
        copy->scope = 0;
        return (INode *)copy;
    }
    case PtrTag:
    {
        StarNode *ptr = (StarNode *)type;
        INode *vtexp = lifeErased(ptr->vtexp);
        if (vtexp == ptr->vtexp)
            return type;
        StarNode *copy = memAllocBlk(sizeof(StarNode));
        memcpy(copy, ptr, sizeof(StarNode));
        copy->vtexp = vtexp;
        return (INode *)copy;
    }
    case TTupleTag:
    {
        TupleNode *tuple = (TupleNode *)type;
        INode **nodesp;
        uint32_t cnt;
        int changed = 0;
        for (nodesFor(tuple->elems, cnt, nodesp)) {
            if (lifeErased(*nodesp) != *nodesp)
                changed = 1;
        }
        if (!changed)
            return type;
        TupleNode *copy = memAllocBlk(sizeof(TupleNode));
        memcpy(copy, tuple, sizeof(TupleNode));
        copy->elems = newNodes(tuple->elems->used);
        for (nodesFor(tuple->elems, cnt, nodesp))
            nodesAdd(&copy->elems, lifeErased(*nodesp));
        return (INode *)copy;
    }
    // A function type's lifetimes are what it promises: part of the type
    default:
        return type;
    }
}

// Does 'longer' last at least as long as 'shorter' by 'order': the same name,
// or reached from it through the order's pairs, '>=' being transitive? The
// unnamed lifetime (NULL) is one more name, ordered only where a pair says so
// (an implied bound, lifeImplied); ''static' outlasts every one, and is
// outlasted only where a type parameter's bound says so (lifeBoundName).
static int lifeOrdered(LifeOrder *order, Name *longer, Name *shorter) {
    if (longer == shorter || longer == staticLifeName)
        return 1;
    if (order == NULL)
        return 0;
    // A walk over a handful of pairs: each name reached is visited once
    Name *localseen[16];
    Name **seen = order->count < 16 ? localseen : memAllocBlk((order->count + 1) * sizeof(Name *));
    uint32_t nseen = 0;
    seen[nseen++] = longer;
    for (uint32_t next = 0; next < nseen; ++next) {
        Name *at = seen[next];
        for (uint32_t i = 0; i < order->count; ++i) {
            if (order->pairs[2 * i] != at)
                continue;
            Name *to = order->pairs[2 * i + 1];
            if (to == shorter)
                return 1;
            uint32_t k;
            for (k = 0; k < nseen && seen[k] != to; ++k)
                ;
            if (k == nseen)
                seen[nseen++] = to;
        }
    }
    return 0;
}

static int lifeOutlives(FnSigNode *sig, Name *longer, Name *shorter, int anon) {
    if (anon)
        return longer == shorter;
    return lifeOrdered(sig ? sig->lifeorder : NULL, longer, shorter);
}

// *********************
// The lifetimes a type holds
// *********************

// A set of lifetimes: each name once, the unnamed one as a flag
typedef struct {
    Name *local[8];
    Name **names;
    uint32_t cnt;
    uint32_t cap;
    uint8_t unnamed;
} LifeSet;

static void lifeSetInit(LifeSet *set) {
    set->names = set->local;
    set->cnt = 0;
    set->cap = 8;
    set->unnamed = 0;
}

static void lifeSetAdd(LifeSet *set, Name *name) {
    if (name == NULL) {
        set->unnamed = 1;
        return;
    }
    // ''static' ties a value to no caller lifetime, and an invariant lifetime
    // to none that ends: a key is no borrow
    if (name == staticLifeName || lifeIsInvariant(name))
        return;
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if (set->names[i] == name)
            return;
    }
    if (set->cnt == set->cap) {
        Name **grown = memAllocBlk(2 * set->cap * sizeof(Name *));
        memcpy(grown, set->names, set->cnt * sizeof(Name *));
        set->names = grown;
        set->cap <<= 1;
    }
    set->names[set->cnt++] = name;
}

static int lifeSetHas(LifeSet *set, Name *name) {
    if (name == NULL)
        return set->unnamed;
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if (set->names[i] == name)
            return 1;
    }
    return 0;
}

// Is this a borrowed reference to a function? A function is never a local, so
// such a borrow is global whatever is written on it (itypeCarriesBorrow).
static int lifeIsFnBorrow(RefNode *ref) {
    return ref->tag == RefTag && isTypeNode(ref->vtexp)
        && itypeGetTypeDcl(ref->vtexp)->tag == FnSigTag;
}

static int lifeIsBorrowRef(INode *typedcl) {
    return (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag || typedcl->tag == VirtRefTag)
        && itypeGetTypeDcl(((RefNode *)typedcl)->region) == borrowRef
        && !lifeIsInvariant(((RefNode *)typedcl)->lifename);
}

StructNode *lifeSlotted(INode *type) {
    if (type == NULL || (isNameUseNode(type) && !isTypeNode(type)))
        return NULL;
    INode *dcl = itypeGetTypeDcl(type);
    return dcl && dcl->tag == StructTag && ((StructNode *)dcl)->lifeparms ? (StructNode *)dcl : NULL;
}

Name *lifeSlotName(INode *use, uint32_t k) {
    while (use->tag == AliasDclTag)
        use = ((AliasDclNode *)use)->target;
    if (use->tag != NameUseTag)
        return NULL;
    NameUseNode *nameuse = (NameUseNode *)use;
    if (nameuse->lifeuse && nameuse->lifeuse->names)
        return k < nameuse->lifeuse->count ? nameuse->lifeuse->names[k] : NULL;
    // 'Self' is the struct with its own lifetimes
    StructNode *strnode = lifeSlotted(use);
    if (nameuse->namesym == selfTypeName && strnode && k < strnode->lifeparms->count)
        return strnode->lifeparms->names[k];
    return NULL;
}

static void lifeGather(INode *type, int anon, LifeSet *set);

// A use of a type: a struct declaring lifetimes holds, where it holds a borrow
// at all, one per slot, as the use names it; a generic instance whose type
// arguments are kept holds what they hold; any other type holding a borrow
// holds it unnamed
static void lifeGatherUse(NameUseNode *use, int anon, LifeSet *set) {
    INode *dcl = itypeGetTypeDcl((INode *)use);
    if (dcl == NULL)
        return;
    LifeUse *lifeuse = use->lifeuse;
    // A bounded type argument, renamed: every borrow it holds is of one name
    if (lifeuse && lifeuse->held) {
        if (itypeCarriesBorrow(dcl))
            lifeSetAdd(set, anon ? NULL : lifeuse->held);
        return;
    }
    if (dcl->tag != StructTag) {
        if (dcl != (INode *)use)
            lifeGather(dcl, anon, set);
        return;
    }
    StructNode *strnode = (StructNode *)dcl;
    if (strnode->lifeparms) {
        if (itypeCarriesBorrow(dcl)) {
            for (uint32_t k = 0; k < strnode->lifeparms->count; ++k)
                lifeSetAdd(set, anon ? NULL : lifeSlotName((INode *)use, k));
        }
    }
    else if (!(lifeuse && lifeuse->typeargs) && itypeCarriesBorrow(dcl))
        lifeSetAdd(set, NULL);
    if (lifeuse && lifeuse->typeargs) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(lifeuse->typeargs, cnt, nodesp))
            lifeGather(*nodesp, anon, set);
    }
}

// Gather the lifetimes a value of this type holds. 'anon' reads it as if no
// lifetime were written: what the signature would promise unannotated.
static void lifeGather(INode *type, int anon, LifeSet *set) {
    if (type == NULL)
        return;
    switch (type->tag) {
    case NameUseTag:
        if (isTypeNode(type))
            lifeGatherUse((NameUseNode *)type, anon, set);
        return;
    case AliasDclTag:
        lifeGather(((AliasDclNode *)type)->target, anon, set);
        return;
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        if (itypeGetTypeDcl(ref->region) == borrowRef) {
            // A key holds no lifetime that ends, nor does what it names,
            // which lives in its arena
            if (lifeIsFnBorrow(ref) || lifeIsInvariant(ref->lifename))
                return;
            lifeSetAdd(set, anon ? NULL : ref->lifename);
        }
        // What a bounded virtual reference points at holds borrows lasting
        // its bound: they may be of that lifetime
        if (ref->bound)
            lifeSetAdd(set, anon ? NULL : ref->bound);
        lifeGather(ref->vtexp, anon, set);
        return;
    }
    case PtrTag:
        lifeGather(((StarNode *)type)->vtexp, anon, set);
        return;
    case ArrayTag:
        lifeGather(arrayElemType(type), anon, set);
        return;
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp))
            lifeGather(*nodesp, anon, set);
        return;
    }
    // A struct reached as a declaration, not through a use, names nothing: any
    // borrow it holds is unnamed. So does any other type holding one.
    default:
        if (itypeCarriesBorrow(type))
            lifeSetAdd(set, NULL);
        return;
    }
}

int lifeHolds(INode *type, Name *life) {
    if (life == staticLifeName)
        return 0;
    LifeSet set;
    lifeSetInit(&set);
    lifeGather(type, 0, &set);
    return lifeSetHas(&set, life);
}

// May a borrow of the lifetime 'life' be held where one of the set's is?
static int lifeNameFlowsSet(FnSigNode *sig, Name *life, LifeSet *to, int anon) {
    if (to->unnamed && lifeOutlives(sig, life, NULL, anon))
        return 1;
    for (uint32_t i = 0; i < to->cnt; ++i) {
        if (lifeOutlives(sig, life, to->names[i], anon))
            return 1;
    }
    return 0;
}

static int lifeSetFlows(FnSigNode *sig, LifeSet *from, LifeSet *to, int anon) {
    if (from->unnamed && lifeNameFlowsSet(sig, NULL, to, anon))
        return 1;
    for (uint32_t i = 0; i < from->cnt; ++i) {
        if (lifeNameFlowsSet(sig, from->names[i], to, anon))
            return 1;
    }
    return 0;
}

static int lifeFlowsAs(FnSigNode *sig, INode *from, INode *to, int anon) {
    LifeSet fromset, toset;
    lifeSetInit(&fromset);
    lifeSetInit(&toset);
    lifeGather(from, anon, &fromset);
    if (fromset.cnt == 0 && !fromset.unnamed)
        return 0;
    lifeGather(to, anon, &toset);
    return lifeSetFlows(sig, &fromset, &toset, anon);
}

int lifeFlows(FnSigNode *sig, INode *from, INode *to) {
    return lifeFlowsAs(sig, from, to, 0);
}

static int lifeNameFlowsAs(FnSigNode *sig, Name *life, INode *to, int anon) {
    LifeSet toset;
    lifeSetInit(&toset);
    lifeGather(to, anon, &toset);
    return lifeNameFlowsSet(sig, anon ? NULL : life, &toset, anon);
}

// *********************
// Parameters and their parts
// *********************

INode *lifeHeld(INode *parmtype) {
    INode *typedcl = itypeGetTypeDcl(parmtype);
    if (lifeIsBorrowRef(typedcl)) {
        if (lifeIsFnBorrow((RefNode *)typedcl))
            return NULL;
        INode *pointee = ((RefNode *)typedcl)->vtexp;
        return itypeCarriesBorrow(pointee) ? pointee : NULL;
    }
    return itypeCarriesBorrow(parmtype) ? parmtype : NULL;
}

int lifeIsOwnBorrow(INode *parmtype) {
    INode *typedcl = itypeGetTypeDcl(parmtype);
    return lifeIsBorrowRef(typedcl) && !lifeIsFnBorrow((RefNode *)typedcl)
        && ((RefNode *)typedcl)->lifename != staticLifeName;
}

int lifeCarry(FnSigNode *sig, INode *parm, INode *to, uint32_t *slots) {
    *slots = LifeAllSlots;
    if (sig == NULL)
        return LifeCarryWhole;
    INode *typedcl = itypeGetTypeDcl(parm);
    int isref = lifeIsBorrowRef(typedcl);
    if (isref) {
        RefNode *ref = (RefNode *)typedcl;
        // A ''static' parameter's argument carries nothing but a global's
        // loans, and a function's borrow none
        if (ref->lifename == staticLifeName || lifeIsFnBorrow(ref))
            return LifeCarryWhole;
        if (lifeNameFlowsAs(sig, ref->lifename, to, 0))
            return LifeCarryWhole;
    }
    INode *held = lifeHeld(parm);
    if (held == NULL)
        return LifeCarryNone;
    StructNode *strnode = lifeSlotted(held);
    if (strnode) {
        uint32_t mask = 0;
        for (uint32_t k = 0; k < strnode->lifeparms->count; ++k) {
            if (lifeNameFlowsAs(sig, lifeSlotName(held, k), to, 0))
                mask |= 1u << k;
        }
        if (mask == 0)
            return LifeCarryNone;
        *slots = mask;
        return LifeCarryHeld;
    }
    if (!lifeFlowsAs(sig, held, to, 0))
        return LifeCarryNone;
    return isref ? LifeCarryHeld : LifeCarryWhole;
}

// Gather the lifetimes of one part of what a parameter lends
static void lifePartGather(INode *parm, uint32_t part, int anon, LifeSet *set) {
    if (part == LifePartOwn) {
        lifeSetAdd(set, anon ? NULL : ((RefNode *)itypeGetTypeDcl(parm))->lifename);
        return;
    }
    INode *held = lifeHeld(parm);
    if (part == LifePartHeld)
        lifeGather(held, anon, set);
    else
        lifeSetAdd(set, anon || held == NULL ? NULL : lifeSlotName(held, part - LifePartSlot));
}

static int lifePartFlowsAs(FnSigNode *sig, INode *parm, uint32_t part, INode *to, int anon) {
    LifeSet partset, toset;
    lifeSetInit(&partset);
    lifeSetInit(&toset);
    lifePartGather(parm, part, anon, &partset);
    lifeGather(to, anon, &toset);
    return lifeSetFlows(sig, &partset, &toset, anon);
}

int lifePartFlows(FnSigNode *sig, INode *parm, uint32_t part, INode *to) {
    return lifePartFlowsAs(sig, parm, part, to, 0);
}

int lifePartFlowsSlots(FnSigNode *sig, INode *parm, uint32_t part, INode *to, uint32_t slots, int all) {
    StructNode *strnode = lifeSlotted(to);
    if (strnode == NULL)
        return lifePartFlows(sig, parm, part, to);
    LifeSet partset;
    lifeSetInit(&partset);
    lifePartGather(parm, part, 0, &partset);
    int any = 0;
    for (uint32_t k = 0; k < strnode->lifeparms->count; ++k) {
        if (!(slots & (1u << k)))
            continue;
        LifeSet slotset;
        lifeSetInit(&slotset);
        lifeSetAdd(&slotset, lifeSlotName(to, k));
        int flows = lifeSetFlows(sig, &partset, &slotset, 0);
        if (flows)
            any = 1;
        else if (all)
            return 0;
    }
    return any;
}

int lifeSlotsApart(FnSigNode *sig, INode *pointee) {
    StructNode *strnode = lifeSlotted(pointee);
    if (strnode == NULL)
        return 1;
    LifeParms *parms = strnode->lifeparms;
    if (parms->count <= 1)
        return 1;
    if (sig == NULL)
        return 0;
    for (uint32_t k = 0; k < parms->count; ++k) {
        Name *named = lifeSlotName(pointee, k);
        for (uint32_t j = 0; j < parms->count; ++j) {
            if (j == k)
                continue;
            Name *other = lifeSlotName(pointee, j);
            if (other == named)
                return 0;
            // Borrows of slot 'j' may go into slot 'k' here: only where the
            // struct says so, so that a read of 'k' carries them anyway
            if (lifeOutlives(sig, other, named, 0) && !(parms->contains[k] & (1u << j)))
                return 0;
        }
    }
    return 1;
}

// *********************
// Structs
// *********************

uint32_t lifeFieldSlots(StructNode *strnode, FieldDclNode *field) {
    if (field->lifeknown)
        return field->lifeslots;
    uint32_t mask = 0;
    LifeParms *parms = strnode->lifeparms;
    if (parms && field->vtype && field->vtype != unknownType) {
        LifeSet set;
        lifeSetInit(&set);
        lifeGather(field->vtype, 0, &set);
        for (uint32_t k = 0; k < parms->count; ++k) {
            if (lifeSetHas(&set, parms->names[k]))
                mask |= 1u << k;
        }
    }
    field->lifeslots = mask;
    field->lifeknown = 1;
    return mask;
}

uint32_t lifeSlotsReach(StructNode *strnode, uint32_t slots) {
    LifeParms *parms = strnode->lifeparms;
    if (parms == NULL)
        return slots;
    uint32_t reach = slots;
    for (uint32_t k = 0; k < parms->count; ++k) {
        if (slots & (1u << k))
            reach |= parms->contains[k];
    }
    return reach;
}

uint32_t lifeFieldTag(StructNode *strnode, FieldDclNode *field) {
    uint32_t slots = lifeFieldSlots(strnode, field);
    if (slots == 0 || (slots & (slots - 1)))
        return 0;
    uint32_t k = 0;
    while (!(slots & (1u << k)))
        ++k;
    return k + 1;
}

// The slot a struct declares a name as, or -1
static int lifeSlotOf(LifeParms *parms, Name *name) {
    for (uint32_t k = 0; k < parms->count; ++k) {
        if (parms->names[k] == name)
            return (int)k;
    }
    return -1;
}

void lifeStructDeclare(StructNode *strnode) {
    LifeParms *parms = strnode->lifeparms;
    if (parms == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    // A struct declaring none takes the one name its fields write
    if (parms->count == 0 && parms->usedat) {
        Name *first = ((NameUseNode *)nodesGet(parms->usedat, 0))->namesym;
        int one = 1;
        for (nodesFor(parms->usedat, cnt, nodesp)) {
            if (((NameUseNode *)*nodesp)->namesym != first)
                one = 0;
        }
        if (one) {
            parms->names = memAllocBlk(sizeof(Name *));
            parms->names[0] = first;
            parms->count = 1;
            parms->inferred = 1;
        }
    }
    // Every other name a field writes is declared, or it is a mistake: a
    // misspelled name would otherwise be one more lifetime, unrelated to all
    Name *reported[LifeMaxSlots + 1];
    uint32_t nreported = 0;
    if (parms->usedat) {
        for (nodesFor(parms->usedat, cnt, nodesp)) {
            Name *name = ((NameUseNode *)*nodesp)->namesym;
            if (lifeSlotOf(parms, name) >= 0)
                continue;
            uint32_t r;
            for (r = 0; r < nreported && reported[r] != name; ++r)
                ;
            if (r < nreported)
                continue;
            if (nreported <= LifeMaxSlots)
                reported[nreported++] = name;
            if (parms->count == 0)
                errorMsgNode(*nodesp, ErrorLifetimeUndeclared,
                    "%s's fields name more than one lifetime, so its brackets declare each: '%s' is not declared ('struct %s['a, 'b] {...}').",
                    &strnode->namesym->namestr, &name->namestr, &strnode->namesym->namestr);
            else
                errorMsgNode(*nodesp, ErrorLifetimeUndeclared,
                    "'%s' is not one of the lifetimes %s declares in its brackets.",
                    &name->namestr, &strnode->namesym->namestr);
        }
        parms->usedat = NULL;
    }
    // Its order is among its own lifetimes
    if (parms->order) {
        for (uint32_t i = 0; i < parms->order->count; ++i) {
            for (int side = 0; side < 2; ++side) {
                Name *name = parms->order->pairs[2 * i + side];
                if (lifeSlotOf(parms, name) < 0)
                    errorMsgNode(parms->order->at[i], ErrorLifetimeUndeclared,
                        "'%s' is not one of the lifetimes %s declares, so its 'where' clause cannot order it.",
                        &name->namestr, &strnode->namesym->namestr);
            }
        }
    }
    // What borrows each slot's places may hold: its own, and, by its order,
    // those of every slot lasting at least as long
    parms->contains = memAllocBlk((parms->count ? parms->count : 1) * sizeof(uint32_t));
    for (uint32_t k = 0; k < parms->count; ++k) {
        parms->contains[k] = 1u << k;
        for (uint32_t j = 0; j < parms->count; ++j) {
            if (j != k && lifeOrdered(parms->order, parms->names[j], parms->names[k]))
                parms->contains[k] |= 1u << j;
        }
    }
    // An enum's variants are written in its names
    if (strnode->derived) {
        for (nodesFor(strnode->derived, cnt, nodesp))
            ((StructNode *)*nodesp)->lifeparms = parms;
    }
}

void lifeStructCheck(StructNode *strnode) {
    // An instance's fields hold what its type arguments do, unnamed or not
    if (strnode->lifeparms == NULL || strnode->instnode)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        if (field->tag != FieldDclTag || (field->flags & (IsMixin | IsTagField)))
            continue;
        if (lifeHolds(field->vtype, NULL))
            errorMsgNode((INode *)field, ErrorLifetimeUndeclared,
                "%s declares its lifetimes, so every borrow it holds names one: '%s' holds a borrow of none ('&'a T', or 'Holder['a]' for a struct holding one).",
                &strnode->namesym->namestr, &field->namesym->namestr);
    }
}

// The kept arguments of a generic instance: only where one holds a borrow
static int lifeArgsHoldBorrow(Nodes *args) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(args, cnt, nodesp)) {
        if (isTypeNode(*nodesp) && itypeCarriesBorrow(*nodesp))
            return 1;
    }
    return 0;
}

// Does one of a generic instance's arguments carry a brand, an invariant
// lifetime the instance keeps, named by its place (lifeCanonBrands)?
static int lifeArgsHoldBrand(Nodes *args) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(args, cnt, nodesp)) {
        if (isTypeNode(*nodesp) && lifeTypeHasBrands(*nodesp))
            return 1;
    }
    return 0;
}

void lifeUseInstance(NameUseNode *instuse, INode *genuse, Nodes *typeargs) {
    LifeUse *genlife = genuse && genuse->tag == NameUseTag ? ((NameUseNode *)genuse)->lifeuse : NULL;
    int keep = typeargs && (lifeArgsHoldBorrow(typeargs) || lifeArgsHoldBrand(typeargs));
    if (genlife == NULL && !keep)
        return;
    LifeUse *lifeuse = memAllocBlk(sizeof(LifeUse));
    lifeuse->names = genlife ? genlife->names : NULL;
    lifeuse->count = genlife ? genlife->count : 0;
    lifeuse->at = genlife ? genlife->at : NULL;
    lifeuse->typeargs = keep ? typeargs : NULL;
    lifeuse->held = NULL;
    instuse->lifeuse = lifeuse;
}

void lifeUseCheck(NameUseNode *use) {
    LifeUse *lifeuse = use->lifeuse;
    if (lifeuse == NULL || lifeuse->names == NULL)
        return;
    INode *dcl = itypeGetTypeDcl((INode *)use);
    StructNode *strnode = dcl && dcl->tag == StructTag ? (StructNode *)dcl : NULL;
    INode *at = lifeuse->at ? lifeuse->at : (INode *)use;
    if (strnode == NULL || strnode->lifeparms == NULL || strnode->lifeparms->count == 0) {
        errorMsgNode(at, ErrorLifetimeArgs, "%s declares no lifetimes, so a use of it names none.",
            &use->namesym->namestr);
        lifeuse->names = NULL;
    }
    else if (strnode->lifeparms->count != lifeuse->count) {
        errorMsgNode(at, ErrorLifetimeArgs, "%s declares %u lifetime%s, and a use names each of them, in order, or none.",
            &use->namesym->namestr, (unsigned)strnode->lifeparms->count, strnode->lifeparms->count == 1 ? "" : "s");
        lifeuse->names = NULL;
    }
    else {
        // An invariant lifetime's place takes an invariant one, and an
        // ordinary one's an ordinary one
        for (uint32_t k = 0; k < lifeuse->count; ++k) {
            if (lifeIsInvariant(strnode->lifeparms->names[k]) != lifeIsInvariant(lifeuse->names[k])) {
                errorMsgNode(at, ErrorLifetimeInvariant,
                    "%s declares '%s' %s, so its use names %s lifetime there, not '%s'.",
                    &use->namesym->namestr, &strnode->lifeparms->names[k]->namestr,
                    lifeIsInvariant(strnode->lifeparms->names[k]) ? "invariant" : "an ordinary lifetime",
                    lifeIsInvariant(strnode->lifeparms->names[k]) ? "an invariant" : "an ordinary",
                    &lifeuse->names[k]->namestr);
                lifeuse->names = NULL;
                return;
            }
        }
    }
}

// *********************
// Signatures
// *********************

// One implied pair, 'longer' >= 'shorter', unless it is one name
static void lifeImpliedAdd(LifeOrder **order, Name *longer, Name *shorter, INode *at) {
    if (longer == shorter)
        return;
    if (*order == NULL)
        *order = newLifeOrder();
    lifeOrderAdd(*order, longer, shorter, at);
}

// Add to 'order' what a type implies of the lifetimes it names: what a struct
// it uses orders, in the names the use gives, and, for a borrow of a value
// holding lifetimes, that each of them outlasts the borrow (Rust's implied
// bounds)
static void lifeImplied(INode *type, LifeOrder **order) {
    if (type == NULL)
        return;
    switch (type->tag) {
    case NameUseTag:
    {
        if (!isTypeNode(type))
            return;
        NameUseNode *use = (NameUseNode *)type;
        StructNode *strnode = lifeSlotted(type);
        if (strnode && strnode->lifeparms->order) {
            LifeOrder *sorder = strnode->lifeparms->order;
            for (uint32_t i = 0; i < sorder->count; ++i) {
                int longer = lifeSlotOf(strnode->lifeparms, sorder->pairs[2 * i]);
                int shorter = lifeSlotOf(strnode->lifeparms, sorder->pairs[2 * i + 1]);
                if (longer < 0 || shorter < 0)
                    continue;
                Name *l = lifeSlotName(type, longer);
                Name *s = lifeSlotName(type, shorter);
                if (l && s)
                    lifeImpliedAdd(order, l, s, type);
            }
        }
        if (use->lifeuse && use->lifeuse->typeargs) {
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(use->lifeuse->typeargs, cnt, nodesp))
                lifeImplied(*nodesp, order);
        }
        return;
    }
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        // A borrow of a value holding lifetimes cannot outlast them: the value
        // lives no longer than any of its borrows, and the borrow no longer
        // than the value. So each lifetime the pointee holds lasts at least as
        // long as the borrow's own. That ''static' outlasts it says nothing.
        RefNode *ref = (RefNode *)type;
        if (itypeGetTypeDcl(ref->region) == borrowRef && !lifeIsFnBorrow(ref)
            && ref->lifename != staticLifeName) {
            LifeSet held;
            lifeSetInit(&held);
            lifeGather(ref->vtexp, 0, &held);
            if (held.unnamed)
                lifeImpliedAdd(order, NULL, ref->lifename, type);
            for (uint32_t i = 0; i < held.cnt; ++i)
                lifeImpliedAdd(order, held.names[i], ref->lifename, type);
            // So does a virtual reference's bound, which what it points at
            // holds: '&<'r Trait + 'a' exists only where ''a' outlasts ''r'
            if (ref->bound && ref->bound != staticLifeName)
                lifeImpliedAdd(order, ref->bound, ref->lifename, type);
        }
        lifeImplied(ref->vtexp, order);
        return;
    }
    case PtrTag:
        lifeImplied(((StarNode *)type)->vtexp, order);
        return;
    case ArrayTag:
        lifeImplied(arrayElemType(type), order);
        return;
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp))
            lifeImplied(*nodesp, order);
        return;
    }
    default:
        return;
    }
}

static int lifeSigNamesBrand(FnSigNode *sig, Name *name);
static void lifeSigBrandsCheck(FnSigNode *sig);

void lifeSigCheck(FnSigNode *sig) {
    if (sig->lifechecked)
        return;
    sig->lifechecked = 1;
    lifeSigBrandsCheck(sig);
    INode **nodesp;
    uint32_t cnt;
    LifeSet named;
    lifeSetInit(&named);
    for (nodesFor(sig->parms, cnt, nodesp))
        lifeGather(((IExpNode *)*nodesp)->vtype, 0, &named);
    lifeGather(sig->rettype, 0, &named);
    if (named.cnt > 0)
        sig->lifenamed = 1;
    // A parameter that is a virtual reference bounded by ''static' takes only
    // a value holding global borrows, which a call checks (pwStaticArgs)
    for (nodesFor(sig->parms, cnt, nodesp)) {
        if (lifeVirtBound(((IExpNode *)*nodesp)->vtype) == staticLifeName)
            sig->lifestatic = 1;
    }
    // A 'where' clause orders the signature's own lifetimes. A type
    // parameter's bound, ''+T' >= ''a', names T's borrows, which its argument
    // may not hold, and a lifetime of the signature's or ''static'.
    LifeOrder *written = sig->lifeorder;
    if (written) {
        for (uint32_t i = 0; i < written->count; ++i) {
            Name *tparm = lifeBoundParm(written->pairs[2 * i]);
            for (int side = 0; side < 2; ++side) {
                Name *name = written->pairs[2 * i + side];
                if (tparm && (side == 0 || name == staticLifeName))
                    continue;
                if (!lifeSetHas(&named, name) && !lifeSigNamesBrand(sig, name)) {
                    if (tparm)
                        errorMsgNode(written->at[i], ErrorLifetimeUndeclared,
                            "'%s' is named by none of the function's parameters or its result, so a bound on %s cannot be of it.",
                            &name->namestr, &tparm->namestr);
                    else
                        errorMsgNode(written->at[i], ErrorLifetimeUndeclared,
                            "'%s' is named by none of the function's parameters or its result, so its 'where' clause cannot order it.",
                            &name->namestr);
                }
            }
            if (tparm && written->pairs[2 * i + 1] == staticLifeName)
                sig->lifestatic = 1;
        }
    }
    // What its types imply holds wherever it is called: a value of
    // 'Pair['x, 'y]' can exist only where ''x' lasts at least as long as ''y'
    // does, and a borrow '&'a Pair['b]' only where ''b' outlasts ''a'
    LifeOrder *implied = NULL;
    for (nodesFor(sig->parms, cnt, nodesp))
        lifeImplied(((IExpNode *)*nodesp)->vtype, &implied);
    lifeImplied(sig->rettype, &implied);
    if (implied) {
        // A fresh order: the written one may be shared with a clone
        if (written) {
            for (uint32_t i = 0; i < written->count; ++i)
                lifeOrderAdd(implied, written->pairs[2 * i], written->pairs[2 * i + 1], written->at[i]);
        }
        sig->lifeorder = implied;
    }
}

// *********************
// Pointees, ''static'
// *********************

INode *lifePointee(INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    if ((typedcl->tag == RefTag || typedcl->tag == ArrayRefTag)
        && itypeGetTypeDcl(((RefNode *)typedcl)->region) == borrowRef)
        return ((RefNode *)typedcl)->vtexp;
    return type;
}

// Is ''static' named anywhere in this type?
static int lifeNamesStatic(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
    {
        LifeUse *lifeuse = ((NameUseNode *)type)->lifeuse;
        if (lifeuse == NULL)
            return 0;
        for (uint32_t k = 0; lifeuse->names && k < lifeuse->count; ++k) {
            if (lifeuse->names[k] == staticLifeName)
                return 1;
        }
        if (lifeuse->typeargs) {
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(lifeuse->typeargs, cnt, nodesp)) {
                if (lifeNamesStatic(*nodesp))
                    return 1;
            }
        }
        return 0;
    }
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        return ((RefNode *)type)->lifename == staticLifeName || lifeNamesStatic(((RefNode *)type)->vtexp);
    case PtrTag:
        return lifeNamesStatic(((StarNode *)type)->vtexp);
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (lifeNamesStatic(*nodesp))
                return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

int lifeParmStaticInside(INode *parmtype) {
    INode *typedcl = itypeGetTypeDcl(parmtype);
    if (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag || typedcl->tag == VirtRefTag)
        return lifeNamesStatic(((RefNode *)typedcl)->vtexp);
    return lifeNamesStatic(parmtype);
}

int lifeIsStatic(INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    return (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag || typedcl->tag == VirtRefTag)
        && ((RefNode *)typedcl)->lifename == staticLifeName;
}

// *********************
// Lifetime bounds
// *********************

int lifeStaticBoundSeen = 0;
int lifeVirtBoundSeen = 0;

Name *lifeBoundName(Name *tparm) {
    char buf[260];
    int len = snprintf(buf, sizeof(buf), "'+%s", &tparm->namestr);
    return nametblFind(buf, (size_t)len);
}

Name *lifeBoundParm(Name *name) {
    if (name == NULL || name->namesz < 3 || (&name->namestr)[0] != '\'' || (&name->namestr)[1] != '+')
        return NULL;
    return nametblFind(&name->namestr + 2, name->namesz - 2);
}

static Name *lifeRenamedName(Name *name, Name *to) {
    return lifeIsInvariant(name) ? name : to;
}

INode *lifeRenamed(INode *type, Name *to) {
    if (type == NULL)
        return type;
    switch (type->tag) {
    case NameUseTag:
    {
        if (!isTypeNode(type))
            return type;
        INode *dcl = itypeGetTypeDcl(type);
        if (dcl == NULL || !itypeCarriesBorrow(dcl))
            return lifeErased(type);
        NameUseNode *use = (NameUseNode *)type;
        LifeUse *old = use->lifeuse;
        LifeUse *renamed = memAllocBlk(sizeof(LifeUse));
        renamed->names = NULL;
        renamed->count = 0;
        renamed->typeargs = NULL;
        renamed->at = old ? old->at : NULL;
        renamed->held = to;
        // A struct's own lifetimes, each the bound's but for an invariant one,
        // which stays the brand it is
        StructNode *strnode = dcl->tag == StructTag ? (StructNode *)dcl : NULL;
        if (strnode && strnode->lifeparms && strnode->lifeparms->count) {
            LifeParms *parms = strnode->lifeparms;
            renamed->count = parms->count;
            renamed->names = memAllocBlk(parms->count * sizeof(Name *));
            for (uint32_t k = 0; k < parms->count; ++k) {
                Name *cur = old && old->names && k < old->count ? old->names[k] : NULL;
                renamed->names[k] = lifeIsInvariant(parms->names[k]) ? cur : to;
            }
        }
        if (old && old->typeargs) {
            INode **nodesp;
            uint32_t cnt;
            renamed->typeargs = newNodes(old->typeargs->used);
            for (nodesFor(old->typeargs, cnt, nodesp))
                nodesAdd(&renamed->typeargs, isTypeNode(*nodesp) ? lifeRenamed(*nodesp, to) : *nodesp);
        }
        NameUseNode *copy = memAllocBlk(sizeof(NameUseNode));
        memcpy(copy, type, sizeof(NameUseNode));
        copy->lifeuse = renamed;
        return (INode *)copy;
    }
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        INode *vtexp = ref->vtexp && isTypeNode(ref->vtexp) ? lifeRenamed(ref->vtexp, to) : ref->vtexp;
        RefNode *copy = memAllocBlk(sizeof(RefNode));
        memcpy(copy, ref, sizeof(RefNode));
        // A function's borrow is global whatever is written on it
        if (itypeGetTypeDcl(ref->region) == borrowRef && !lifeIsFnBorrow(ref))
            copy->lifename = lifeRenamedName(ref->lifename, to);
        if (ref->bound)
            copy->bound = to;
        copy->vtexp = vtexp;
        // An argument inferred from a borrow carries the caller's band, which
        // is no lifetime of the instance's: as a written type argument has
        // none, it has none; the name says what it outlasts
        copy->scope = 0;
        return (INode *)copy;
    }
    case PtrTag:
    {
        StarNode *ptr = (StarNode *)type;
        INode *vtexp = lifeRenamed(ptr->vtexp, to);
        if (vtexp == ptr->vtexp)
            return type;
        StarNode *copy = memAllocBlk(sizeof(StarNode));
        memcpy(copy, ptr, sizeof(StarNode));
        copy->vtexp = vtexp;
        return (INode *)copy;
    }
    case TTupleTag:
    {
        TupleNode *tuple = (TupleNode *)type;
        TupleNode *copy = memAllocBlk(sizeof(TupleNode));
        memcpy(copy, tuple, sizeof(TupleNode));
        copy->elems = newNodes(tuple->elems->used);
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(tuple->elems, cnt, nodesp))
            nodesAdd(&copy->elems, lifeRenamed(*nodesp, to));
        return (INode *)copy;
    }
    default:
        return lifeErased(type);
    }
}

int lifeParmBounded(INode *generic, Name *tparm) {
    if (generic == NULL || generic->tag != FnDclTag)
        return 0;
    FnSigNode *sig = (FnSigNode *)((FnDclNode *)generic)->vtype;
    if (sig == NULL || sig->tag != FnSigTag || sig->lifeorder == NULL)
        return 0;
    Name *bound = lifeBoundName(tparm);
    for (uint32_t i = 0; i < sig->lifeorder->count; ++i) {
        if (sig->lifeorder->pairs[2 * i] == bound)
            return 1;
    }
    return 0;
}

int lifeParmStaticBounded(INode *generic, Name *tparm) {
    if (generic == NULL || generic->tag != FnDclTag)
        return 0;
    FnSigNode *sig = (FnSigNode *)((FnDclNode *)generic)->vtype;
    if (sig == NULL || sig->tag != FnSigTag || sig->lifeorder == NULL)
        return 0;
    Name *bound = lifeBoundName(tparm);
    for (uint32_t i = 0; i < sig->lifeorder->count; ++i) {
        if (sig->lifeorder->pairs[2 * i] == bound && sig->lifeorder->pairs[2 * i + 1] == staticLifeName)
            return 1;
    }
    return 0;
}

// Is 'tparm' one of a generic declaration's own type parameters?
static int lifeIsTypeParm(GenericInfo *info, Name *tparm) {
    if (info == NULL || info->parms == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(info->parms, cnt, nodesp)) {
        if (((GenVarDclNode *)*nodesp)->namesym == tparm)
            return 1;
    }
    return 0;
}

void lifeBoundsNameRes(FnDclNode *fndcl, INode *owner) {
    FnSigNode *sig = (FnSigNode *)fndcl->vtype;
    if (sig == NULL || sig->tag != FnSigTag || sig->lifeorder == NULL)
        return;
    LifeOrder *order = sig->lifeorder;
    uint32_t keep = 0;
    for (uint32_t i = 0; i < order->count; ++i) {
        Name *tparm = lifeBoundParm(order->pairs[2 * i]);
        if (tparm && !lifeIsTypeParm(fndcl->genericinfo, tparm)) {
            // A generic type's parameter is the type's instance's, whose
            // methods are made from arguments erased, not renamed
            if (owner && owner->tag == StructTag && lifeIsTypeParm(((StructNode *)owner)->genericinfo, tparm))
                errorMsgNode(order->at[i], ErrorLifetimeBound,
                    "%s is a type parameter of the generic type, and a lifetime bound on one is not built: a generic function's own type parameter takes one.",
                    &tparm->namestr);
            else
                errorMsgNode(order->at[i], ErrorWhereSubject,
                    "%s is not a type parameter of this function, so it takes no lifetime bound.",
                    &tparm->namestr);
            continue;
        }
        order->pairs[2 * keep] = order->pairs[2 * i];
        order->pairs[2 * keep + 1] = order->pairs[2 * i + 1];
        order->at[keep++] = order->at[i];
    }
    order->count = keep;
}

int lifePartStatic(FnSigNode *sig, INode *parm, uint32_t part) {
    if (sig == NULL || !sig->lifestatic)
        return 0;
    LifeSet set;
    lifeSetInit(&set);
    lifePartGather(parm, part, 0, &set);
    if (set.unnamed || set.cnt == 0)
        return 0;
    for (uint32_t i = 0; i < set.cnt; ++i) {
        if (!lifeOrdered(sig->lifeorder, set.names[i], staticLifeName))
            return 0;
    }
    return 1;
}

Name *lifeStaticBoundOf(FnSigNode *sig, INode *parm) {
    if (sig == NULL || !sig->lifestatic)
        return NULL;
    LifeSet set;
    lifeSetInit(&set);
    lifeGather(parm, 0, &set);
    for (uint32_t i = 0; i < set.cnt; ++i) {
        Name *tparm = lifeBoundParm(set.names[i]);
        if (tparm && lifeOrdered(sig->lifeorder, set.names[i], staticLifeName))
            return tparm;
    }
    return NULL;
}

Name *lifeVirtBound(INode *type) {
    if (type == NULL || !isTypeNode(type))
        return NULL;
    INode *dcl = itypeGetTypeDcl(type);
    return dcl && dcl->tag == VirtRefTag ? ((RefNode *)dcl)->bound : NULL;
}

int lifeVirtOutlives(FnSigNode *sig, INode *vreftype, Name *bound) {
    INode *dcl = itypeGetTypeDcl(vreftype);
    if (dcl == NULL || dcl->tag != VirtRefTag)
        return 1;
    RefNode *ref = (RefNode *)dcl;
    return lifeOutlives(sig, ref->bound ? ref->bound : ref->lifename, bound, 0);
}

int lifePartOutlives(FnSigNode *sig, INode *parm, uint32_t part, Name *bound) {
    LifeSet set;
    lifeSetInit(&set);
    lifePartGather(parm, part, 0, &set);
    if (set.unnamed && !lifeOutlives(sig, NULL, bound, 0))
        return 0;
    for (uint32_t i = 0; i < set.cnt; ++i) {
        if (!lifeOutlives(sig, set.names[i], bound, 0))
            return 0;
    }
    return 1;
}

// *********************
// What a signature promises
// *********************

// May a call store a value through this parameter: is it a writable borrowed
// reference to something that can hold a borrow?
static int lifeParmStores(INode *parmtype) {
    RefNode *ref = (RefNode *)itypeGetTypeDcl(parmtype);
    return (ref->tag == RefTag || ref->tag == ArrayRefTag) && itypeGetTypeDcl(ref->region) == borrowRef
        && (permGetFlags(ref->perm) & MayWrite) && itypeCarriesBorrow(ref->vtexp);
}

// The parts a caller lends through a parameter, as LifePart values, into
// 'parts' (room for LifeMaxSlots + 2); their number
static uint32_t lifeParts(INode *parm, uint32_t *parts) {
    uint32_t n = 0;
    if (lifeIsOwnBorrow(parm))
        parts[n++] = LifePartOwn;
    INode *held = lifeHeld(parm);
    if (held) {
        StructNode *strnode = lifeSlotted(held);
        if (strnode) {
            for (uint32_t k = 0; k < strnode->lifeparms->count; ++k)
                parts[n++] = LifePartSlot + k;
        }
        else
            parts[n++] = LifePartHeld;
    }
    return n;
}

// What each digit of a signature's promises says, for comparing two
// signatures one digit at a time (lifeSigMeets)
enum LifePromiseKind {
    LifePromiseParm = 'p',      // a parameter's first: flows (1), ''static' (2), stored through (4)
    LifePromiseFlows = 'f',     // flows (1) or not (0)
    LifePromiseApart = 'a',     // the slots a writable parameter points at named apart (1) or not
};

// A signature's promises as they are written: the digits, and, where asked,
// what each says (LifePromiseKind)
typedef struct {
    char *p;
    char *k;
} LifePromises;

static void lifePromise(LifePromises *out, int digit, char kind) {
    *out->p++ = (char)('0' + digit);
    if (out->k)
        *out->k++ = kind;
}

// Append, for a parameter, whether a value of the type 'to' may hold each of
// its parts: one digit for any, and, where 'detail' asks and it has several,
// one per part
static void lifePromiseParts(LifePromises *out, FnSigNode *sig, INode *parm, INode *to, int anon, int extra,
        char kind, int detail) {
    uint32_t parts[LifeMaxSlots + 2];
    uint32_t n = lifeParts(parm, parts);
    int any = lifeFlowsAs(sig, parm, to, anon);
    lifePromise(out, any + extra, kind);
    if (detail && n > 1) {
        for (uint32_t i = 0; i < n; ++i)
            lifePromise(out, lifePartFlowsAs(sig, parm, parts[i], to, anon), LifePromiseFlows);
    }
}

// What a signature promises about lifetimes, as a string of digits: for each
// parameter, whether the result may hold what it lends (1), whether its
// reference is ''static' (2) and whether a call may store through it (4), and
// after that, with 'detail', for a parameter lending several parts, whether
// the result may hold each; after each parameter a call may store through,
// for every other parameter, whether what it lends may be stored there, as
// for the result, and, with 'detail', whether the slots of the struct it
// points at are named apart. 'anon' reads it as if no lifetime were written.
// A call is checked against nothing else, so two signatures promising the
// same agree. Where 'kinds' is given, it gets what each digit says.
static char *lifeSigPromises(FnSigNode *sig, int anon, int detail, char **kinds) {
    uint32_t nparms = sig->parms->used;
    size_t size = (nparms + 1) * (nparms + 1) * (LifeMaxSlots + 3) + 1;
    char *promises = memAllocStr(NULL, size);
    LifePromises out = { promises, kinds ? (*kinds = memAllocStr(NULL, size)) : NULL };
    for (uint32_t i = 0; i < nparms; ++i) {
        INode *parmtype = ((IExpNode *)nodesGet(sig->parms, i))->vtype;
        // A call can store only what another parameter lends, so a lone
        // parameter, a cursor's 'self &mut', is never stored through
        int stores = nparms > 1 && lifeParmStores(parmtype);
        lifePromiseParts(&out, sig, parmtype, sig->rettype, anon,
            (!anon && lifeIsStatic(parmtype) ? 2 : 0) + (stores ? 4 : 0), LifePromiseParm, detail);
        if (!stores)
            continue;
        INode *pointee = lifePointee(parmtype);
        for (uint32_t k = 0; k < nparms; ++k) {
            if (k != i)
                lifePromiseParts(&out, sig, ((IExpNode *)nodesGet(sig->parms, k))->vtype, pointee, anon, 0,
                    LifePromiseFlows, detail);
        }
        if (detail && lifeSlotted(pointee))
            lifePromise(&out, anon ? lifeSlotsApart(NULL, pointee) : lifeSlotsApart(sig, pointee), LifePromiseApart);
    }
    *out.p = '\0';
    if (out.k)
        *out.k = '\0';
    return promises;
}

// How many parts each parameter lends, as a string: two signatures whose
// parameters lend alike are compared part by part
static char *lifeSigShape(FnSigNode *sig) {
    uint32_t nparms = sig->parms->used;
    char *shape = memAllocStr(NULL, nparms + 1);
    for (uint32_t i = 0; i < nparms; ++i) {
        uint32_t parts[LifeMaxSlots + 2];
        uint32_t n = lifeParts(((IExpNode *)nodesGet(sig->parms, i))->vtype, parts);
        shape[i] = (char)('0' + n);
    }
    shape[nparms] = '\0';
    return shape;
}

// Two signatures agree where each parameter's lending flows alike. Where
// their parameters lend different parts -- a trait's 'Self', and a struct's
// declaring lifetimes that implements it -- they are compared whole parameter
// by whole parameter: a call through the one lending fewer parts carries an
// argument whole wherever the other carries any part of it, the cautious side.
static int lifeSigBrandsAgree(FnSigNode *a, FnSigNode *b);

int lifeSigsAgree(FnSigNode *a, FnSigNode *b) {
    if (!lifeSigBrandsAgree(a, b))
        return 0;
    if (!a->lifenamed && !b->lifenamed)
        return 1;
    if (a->parms->used != b->parms->used)
        return 0;
    int detail = strcmp(lifeSigShape(a), lifeSigShape(b)) == 0;
    return strcmp(lifeSigPromises(a, !a->lifenamed, detail, NULL), lifeSigPromises(b, !b->lifenamed, detail, NULL)) == 0;
}

// A call made through 'req' carries what 'req' says may flow, and is checked
// against what it requires, so an implementation 'impl' meets it where it
// promises at least as much, digit by digit: its result, and what it stores
// through a writable parameter, holds no part 'req''s may not (a result of
// ''static', or of a longer lifetime than 'req''s); a parameter is ''static'
// only where 'req''s is, so it takes any argument 'req''s takes (a shorter
// one than 'req' requires, too); one is stored through where 'req''s is; and
// the slots of a struct one points at are named apart wherever 'req''s are.
int lifeSigMeets(FnSigNode *impl, FnSigNode *req) {
    // Brands are identities: an implementation promises exactly the
    // requirement's, neither more nor less
    if (!lifeSigBrandsAgree(impl, req))
        return 0;
    if (!impl->lifenamed && !req->lifenamed)
        return 1;
    if (impl->parms->used != req->parms->used)
        return 0;
    int detail = strcmp(lifeSigShape(impl), lifeSigShape(req)) == 0;
    char *ikinds, *rkinds;
    char *ip = lifeSigPromises(impl, !impl->lifenamed, detail, &ikinds);
    char *rp = lifeSigPromises(req, !req->lifenamed, detail, &rkinds);
    if (strcmp(ikinds, rkinds) != 0)
        return 0;
    for (size_t i = 0; ip[i]; ++i) {
        int d = ip[i] - '0';
        int r = rp[i] - '0';
        switch (ikinds[i]) {
        case LifePromiseParm:
            if ((d & 4) != (r & 4) || ((d & 2) && !(r & 2)) || ((d & 1) && !(r & 1)))
                return 0;
            break;
        case LifePromiseApart:
            if (d < r)
                return 0;
            break;
        default:
            if (d > r)
                return 0;
            break;
        }
    }
    return 1;
}

char *lifeSigSpell(char *bufp, FnSigNode *sig) {
    if (!sig->lifenamed)
        return bufp;
    char *promises = lifeSigPromises(sig, 0, 1, NULL);
    if (strcmp(promises, lifeSigPromises(sig, 1, 1, NULL)) == 0)
        return bufp;
    *bufp++ = 'G';
    size_t len = strlen(promises);
    memcpy(bufp, promises, len);
    bufp += len;
    *bufp++ = '_';
    return bufp;
}

// *********************
// Invariant lifetimes: brands
// *********************

int lifeInvariantSeen = 0;

int lifeIsInvariant(Name *name) {
    return name != NULL && name->namesz >= 2 && (&name->namestr)[0] == '\'' && (&name->namestr)[1] == '=';
}

int lifeIsKey(INode *type) {
    if (type == NULL || !isTypeNode(type))
        return 0;
    INode *dcl = itypeGetTypeDcl(type);
    return dcl != NULL && dcl->tag == RefTag && lifeIsInvariant(((RefNode *)dcl)->lifename);
}

// The brands a type carries, in the order its parts are walked: a NULL
// entry is a place for one that carries none (a struct's invariant lifetime
// its use leaves unnamed)
typedef struct LifeBrands {
    Name *local[16];
    Name **names;
    uint32_t cnt;
    uint32_t cap;
} LifeBrands;

static void lifeBrandsInit(LifeBrands *brands) {
    brands->names = brands->local;
    brands->cnt = 0;
    brands->cap = 16;
}

static void lifeBrandsAdd(LifeBrands *brands, Name *name) {
    if (brands->cnt == brands->cap) {
        Name **grown = memAllocBlk(2 * brands->cap * sizeof(Name *));
        memcpy(grown, brands->names, brands->cnt * sizeof(Name *));
        brands->names = grown;
        brands->cap <<= 1;
    }
    brands->names[brands->cnt++] = name;
}

static int lifeBrandsHas(LifeBrands *brands, Name *name) {
    for (uint32_t i = 0; i < brands->cnt; ++i) {
        if (brands->names[i] == name)
            return 1;
    }
    return 0;
}

static void lifeBrandsOf(INode *type, LifeBrands *brands, int depth);

// A struct's: one per invariant lifetime it declares, as 'use' names it (the
// struct's own names where 'use' is NULL or 'Self'), then its type arguments'
static void lifeBrandsOfStruct(StructNode *strnode, NameUseNode *use, LifeBrands *brands, int depth) {
    LifeParms *parms = strnode->lifeparms;
    if (parms) {
        for (uint32_t k = 0; k < parms->count; ++k) {
            if (lifeIsInvariant(parms->names[k]))
                lifeBrandsAdd(brands, use ? lifeSlotName((INode *)use, k) : parms->names[k]);
        }
    }
    Nodes *typeargs = use && use->lifeuse ? use->lifeuse->typeargs : NULL;
    if (typeargs == NULL)
        typeargs = itypeInstanceTypeArgs((INode *)strnode);
    if (typeargs) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(typeargs, cnt, nodesp))
            lifeBrandsOf(*nodesp, brands, depth + 1);
    }
}

static void lifeBrandsOf(INode *type, LifeBrands *brands, int depth) {
    if (type == NULL || depth > 16)
        return;
    switch (type->tag) {
    case NameUseTag:
    {
        if (!isTypeNode(type))
            return;
        INode *dcl = itypeGetTypeDcl(type);
        if (dcl == NULL)
            return;
        if (dcl->tag == StructTag)
            lifeBrandsOfStruct((StructNode *)dcl, (NameUseNode *)type, brands, depth);
        else if (dcl != type)
            lifeBrandsOf(dcl, brands, depth + 1);
        return;
    }
    case AliasDclTag:
        lifeBrandsOf(((AliasDclNode *)type)->target, brands, depth + 1);
        return;
    case StructTag:
        lifeBrandsOfStruct((StructNode *)type, NULL, brands, depth);
        return;
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        if (lifeIsInvariant(ref->lifename))
            lifeBrandsAdd(brands, ref->lifename);
        if (ref->vtexp && isTypeNode(ref->vtexp))
            lifeBrandsOf(ref->vtexp, brands, depth + 1);
        return;
    }
    // A raw pointer is checked by nothing: it carries no brand, whatever it
    // points at, and a region makes its keys from one (a cast)
    case PtrTag:
        return;
    case ArrayTag:
        lifeBrandsOf(arrayElemType(type), brands, depth + 1);
        return;
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp))
            lifeBrandsOf(*nodesp, brands, depth + 1);
        return;
    }
    default:
        return;
    }
}

int lifeTypeHasBrands(INode *type) {
    if (!lifeInvariantSeen || type == NULL)
        return 0;
    LifeBrands brands;
    lifeBrandsInit(&brands);
    lifeBrandsOf(type, &brands, 0);
    return brands.cnt > 0;
}

// Does one of a signature's types name this invariant lifetime?
static int lifeSigNamesBrand(FnSigNode *sig, Name *name) {
    if (!lifeIsInvariant(name))
        return 0;
    LifeBrands brands;
    lifeBrandsInit(&brands);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp))
        lifeBrandsOf(((IExpNode *)*nodesp)->vtype, &brands, 0);
    lifeBrandsOf(sig->rettype, &brands, 0);
    return lifeBrandsHas(&brands, name);
}

// A struct declaring an invariant lifetime is named with one wherever a
// signature uses it: left unnamed, its keys would be of no brand anything knows
static void lifeSigBrandsCheck(FnSigNode *sig) {
    if (!lifeInvariantSeen)
        return;
    for (uint32_t i = 0; i <= sig->parms->used; ++i) {
        INode *type = i < sig->parms->used ? ((IExpNode *)nodesGet(sig->parms, i))->vtype : sig->rettype;
        LifeBrands brands;
        lifeBrandsInit(&brands);
        lifeBrandsOf(type, &brands, 0);
        if (lifeBrandsHas(&brands, NULL)) {
            errorMsgNode(type, ErrorLifetimeInvariant,
                "A struct declaring an invariant lifetime is named with one in a signature, 'Graph['=a]': unnamed, it would be of no brand anything knows.");
            return;
        }
    }
}

// A signature's brands as a pattern: for each place, the first place naming
// the same brand (one name, or two its 'where' clause equates)
static void lifeSigBrandPattern(FnSigNode *sig, LifeBrands *brands, uint32_t *pattern) {
    for (uint32_t i = 0; i < brands->cnt; ++i) {
        pattern[i] = i;
        Name *name = brands->names[i];
        for (uint32_t j = 0; j < i; ++j) {
            Name *other = brands->names[j];
            if (name == other || (name && other && sig->lifeorder
                && lifeOrdered(sig->lifeorder, name, other) && lifeOrdered(sig->lifeorder, other, name))) {
                pattern[i] = pattern[j];
                break;
            }
        }
    }
}

// Do two signatures say the same of their brands: which of their places carry
// one brand, and which a brand only the result names (minted by a call)? A
// call through the one is checked by what the other promises.
static int lifeSigBrandsAgree(FnSigNode *a, FnSigNode *b) {
    if (!lifeInvariantSeen)
        return 1;
    LifeBrands ba, bb;
    lifeBrandsInit(&ba);
    lifeBrandsInit(&bb);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(a->parms, cnt, nodesp))
        lifeBrandsOf(((IExpNode *)*nodesp)->vtype, &ba, 0);
    uint32_t aparms = ba.cnt;
    lifeBrandsOf(a->rettype, &ba, 0);
    for (nodesFor(b->parms, cnt, nodesp))
        lifeBrandsOf(((IExpNode *)*nodesp)->vtype, &bb, 0);
    uint32_t bparms = bb.cnt;
    lifeBrandsOf(b->rettype, &bb, 0);
    if (ba.cnt == 0 && bb.cnt == 0)
        return 1;
    if (ba.cnt != bb.cnt || aparms != bparms)
        return 0;
    uint32_t *pa = memAllocBlk(ba.cnt * sizeof(uint32_t));
    uint32_t *pb = memAllocBlk(bb.cnt * sizeof(uint32_t));
    lifeSigBrandPattern(a, &ba, pa);
    lifeSigBrandPattern(b, &bb, pb);
    for (uint32_t i = 0; i < ba.cnt; ++i) {
        if (pa[i] != pb[i] || (ba.names[i] == NULL) != (bb.names[i] == NULL))
            return 0;
    }
    return 1;
}

int lifeSigHasBrands(FnSigNode *sig) {
    if (!lifeInvariantSeen)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        if (lifeTypeHasBrands(((IExpNode *)*nodesp)->vtype))
            return 1;
    }
    return lifeTypeHasBrands(sig->rettype);
}

// *********************
// Minted brands, and the loops they are minted in
// *********************

// A loop whose body is being checked, inside the one around it
typedef struct LifeLoop {
    INode *loop;
    struct LifeLoop *outer;
} LifeLoop;

// A brand a call or a construction minted: where, and in which loop's pass
typedef struct LifeMint {
    Name *name;
    INode *at;
    LifeLoop *loop;
} LifeMint;

static LifeMint *lifeMints = NULL;
static uint32_t lifeMintCnt = 0;
static uint32_t lifeMintCap = 0;
static uint32_t lifeMintSeq = 0;
static LifeLoop *brandLoop = NULL;

static int lifeIsCanon(Name *name);

static LifeMint *lifeMintOf(Name *name) {
    if (name == NULL)
        return NULL;
    for (uint32_t i = lifeMintCnt; i-- > 0;) {
        if (lifeMints[i].name == name)
            return &lifeMints[i];
    }
    return NULL;
}

// A fresh brand, minted at 'at' for the callee's (or struct's) name 'from':
// spelled ''=a#n', which no source can write, so it is equal to nothing else
static Name *lifeBrandMint(Name *from, INode *at) {
    char buf[128];
    char *base = from && from != unknownBrandName ? &from->namestr : "'=";
    int len = snprintf(buf, sizeof(buf), "%.100s#%u", base, ++lifeMintSeq);
    Name *name = nametblFind(buf, (size_t)len);
    if (lifeMintCnt == lifeMintCap) {
        uint32_t cap = lifeMintCap ? lifeMintCap << 1 : 32;
        LifeMint *grown = memAllocBlk(cap * sizeof(LifeMint));
        if (lifeMintCnt)
            memcpy(grown, lifeMints, lifeMintCnt * sizeof(LifeMint));
        lifeMints = grown;
        lifeMintCap = cap;
    }
    lifeMints[lifeMintCnt].name = name;
    lifeMints[lifeMintCnt].at = at;
    lifeMints[lifeMintCnt].loop = brandLoop;
    ++lifeMintCnt;
    return name;
}

void lifeBrandLoopEnter(INode *loop) {
    LifeLoop *frame = memAllocBlk(sizeof(LifeLoop));
    frame->loop = loop;
    frame->outer = brandLoop;
    brandLoop = frame;
}

void lifeBrandLoopExit() {
    if (brandLoop)
        brandLoop = brandLoop->outer;
}

// Is the loop 'frame' still being checked, around what is checked now?
static int lifeLoopOpen(LifeLoop *frame) {
    for (LifeLoop *open = brandLoop; open; open = open->outer) {
        if (open == frame)
            return 1;
    }
    return 0;
}

// The loop, closed since, whose pass minted this brand, or NULL
static LifeLoop *lifeBrandClosedLoop(Name *name) {
    LifeMint *mint = lifeMintOf(name);
    if (mint == NULL)
        return NULL;
    for (LifeLoop *frame = mint->loop; frame; frame = frame->outer) {
        if (!lifeLoopOpen(frame))
            return frame;
    }
    return NULL;
}

// *********************
// The function being checked, and the bindings of a call
// *********************

// The current function: the names its parameters write, compared by
// identity, and a binding, made once for the whole function, of each other
// name its types write -- its result's, which a call mints -- to the brand
// its body first gives it
typedef struct LifeBrandFn {
    FnSigNode *sig;
    LifeBrands parmnames;
    LifeBrands resultnames;
    Name **from;
    Name **to;
    uint32_t cnt;
    uint32_t cap;
} LifeBrandFn;

struct LifeBind {
    FnSigNode *sig;     // the callee's, for its 'where' clause; NULL for a struct's
    Name **from;
    Name **to;
    uint32_t cnt;
    uint32_t cap;
    uint32_t seq;       // positional: how many brands named so far
    uint8_t positional; // each brand met is named afresh, by its place (lifeCanonBrands)
    LifeBind *outer;
};

static void lifeBindInit(LifeBind *bind) {
    bind->sig = NULL;
    bind->from = bind->to = NULL;
    bind->cnt = bind->cap = 0;
    bind->seq = 0;
    bind->positional = 0;
    bind->outer = NULL;
}

static LifeBrandFn *brandFn = NULL;
static LifeBind *brandBind = NULL;

static void lifePairAdd(Name ***from, Name ***to, uint32_t *cnt, uint32_t *cap, Name *f, Name *t) {
    if (*cnt == *cap) {
        uint32_t newcap = *cap ? *cap << 1 : 8;
        Name **nf = memAllocBlk(newcap * sizeof(Name *));
        Name **nt = memAllocBlk(newcap * sizeof(Name *));
        if (*cnt) {
            memcpy(nf, *from, *cnt * sizeof(Name *));
            memcpy(nt, *to, *cnt * sizeof(Name *));
        }
        *from = nf;
        *to = nt;
        *cap = newcap;
    }
    (*from)[*cnt] = f;
    (*to)[(*cnt)++] = t;
}

static Name *lifePairFind(Name **from, Name **to, uint32_t cnt, Name *f) {
    for (uint32_t i = 0; i < cnt; ++i) {
        if (from[i] == f)
            return to[i];
    }
    return NULL;
}

void lifeBrandFnBegin(FnSigNode *sig, LifeBrandSave *save) {
    save->fn = brandFn;
    save->bind = brandBind;
    save->loop = brandLoop;
    brandBind = NULL;
    brandLoop = NULL;
    brandFn = NULL;
    if (!lifeInvariantSeen || sig == NULL)
        return;
    LifeBrandFn *fn = memAllocBlk(sizeof(LifeBrandFn));
    fn->sig = sig;
    lifeBrandsInit(&fn->parmnames);
    lifeBrandsInit(&fn->resultnames);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp))
        lifeBrandsOf(((IExpNode *)*nodesp)->vtype, &fn->parmnames, 0);
    lifeBrandsOf(sig->rettype, &fn->resultnames, 0);
    fn->from = fn->to = NULL;
    fn->cnt = fn->cap = 0;
    brandFn = fn;
}

void lifeBrandFnEnd(LifeBrandSave *save) {
    brandFn = (LifeBrandFn *)save->fn;
    brandBind = save->bind;
    brandLoop = (LifeLoop *)save->loop;
}

// A brand as the current function knows it: a name bound for the whole
// function stands for the brand bound to it
static Name *lifeBrandResolve(Name *name) {
    if (brandFn && name) {
        Name *to = lifePairFind(brandFn->from, brandFn->to, brandFn->cnt, name);
        if (to)
            return to;
    }
    return name;
}

// Two brands are the same where they are one name, or the current function's
// 'where' clause equates them. The unknown brand is the same as none.
static int lifeBrandSame(Name *a, Name *b) {
    if (a == NULL || b == NULL)
        return a == b;
    a = lifeBrandResolve(a);
    b = lifeBrandResolve(b);
    if (a == unknownBrandName || b == unknownBrandName)
        return 0;
    if (a == b)
        return 1;
    LifeOrder *order = brandFn && brandFn->sig ? brandFn->sig->lifeorder : NULL;
    return order != NULL && lifeOrdered(order, a, b) && lifeOrdered(order, b, a);
}

// May the current function bind this name for itself: one its result writes
// and its parameters do not, the brand a call of it mints?
static int lifeBrandFnBindable(Name *name) {
    return brandFn != NULL && name != NULL && name != unknownBrandName && !lifeIsCanon(name)
        && lifeBrandsHas(&brandFn->resultnames, name)
        && !lifeBrandsHas(&brandFn->parmnames, name)
        && lifePairFind(brandFn->from, brandFn->to, brandFn->cnt, name) == NULL;
}

// Do two brands agree: the same, or one a name of the current function's
// result not yet bound, which is bound to the other here, once for the whole
// function -- the brand its body gives what it returns
static int lifeBrandAgree(Name *a, Name *b) {
    if (a == NULL || b == NULL)
        return a == b;
    a = lifeBrandResolve(a);
    b = lifeBrandResolve(b);
    if (lifeBrandSame(a, b))
        return 1;
    if (a == unknownBrandName || b == unknownBrandName)
        return 0;
    if (lifeBrandFnBindable(a)) {
        lifePairAdd(&brandFn->from, &brandFn->to, &brandFn->cnt, &brandFn->cap, a, b);
        return 1;
    }
    if (lifeBrandFnBindable(b)) {
        lifePairAdd(&brandFn->from, &brandFn->to, &brandFn->cnt, &brandFn->cap, b, a);
        return 1;
    }
    return 0;
}

LifeBind *lifeBindBegin(FnSigNode *sig) {
    LifeBind *bind = memAllocBlk(sizeof(LifeBind));
    lifeBindInit(bind);
    bind->sig = sig;
    bind->outer = brandBind;
    brandBind = bind;
    return bind;
}

void lifeBindEnd(LifeBind *bind) {
    if (brandBind == bind)
        brandBind = bind->outer;
}

// Does the wanted brand 'want' take the given one 'given'? Under a binding, a
// name of the callee's is bound at its first meeting and compared after;
// otherwise as the current function's (lifeBrandFnBindable).
static int lifeBrandMatch(LifeBind *bind, Name *want, Name *given) {
    if (want == NULL || given == NULL)
        return want == given;
    if (bind) {
        Name *bound = lifePairFind(bind->from, bind->to, bind->cnt, want);
        if (bound)
            return lifeBrandAgree(bound, given);
        lifePairAdd(&bind->from, &bind->to, &bind->cnt, &bind->cap, want, lifeBrandResolve(given));
        return 1;
    }
    return lifeBrandAgree(want, given);
}

// How a brand reads in a message
static char *lifeBrandSpell(Name *name, char *buf, size_t size) {
    if (name == NULL) {
        snprintf(buf, size, "no invariant lifetime");
        return buf;
    }
    name = lifeBrandResolve(name);
    LifeMint *mint = lifeMintOf(name);
    if (mint)
        snprintf(buf, size, "the brand minted at %u:%u", mint->at->linenbr,
            (uint32_t)(mint->at->srcp - mint->at->linep) + 1);
    else if (name == unknownBrandName)
        snprintf(buf, size, "a brand not known here");
    else
        snprintf(buf, size, "'%s'", &name->namestr);
    return buf;
}

void lifeKeyAccessError(INode *at, INode *keytype) {
    char buf[96];
    errorMsgNode(at, ErrorKeyAccess,
        "This is a key, of %s: it gives no access on its own. Reach what it names through its arena, 'arena[key]'.",
        lifeBrandSpell(((RefNode *)itypeGetTypeDcl(keytype))->lifename, buf, sizeof(buf)));
}

static void lifeBrandError(INode *at, Name *want, Name *given) {
    char wantbuf[96], givenbuf[96];
    LifeLoop *closed = given ? lifeBrandClosedLoop(lifeBrandResolve(given)) : NULL;
    LifeMint *mint = given ? lifeMintOf(lifeBrandResolve(given)) : NULL;
    if (mint && mint->loop && lifeLoopOpen(mint->loop) && brandBind == NULL && want) {
        // Minted in a loop still open, and stored where a brand from outside
        // its pass is, which a later pass would read
        LifeMint *wmint = want ? lifeMintOf(lifeBrandResolve(want)) : NULL;
        if (wmint == NULL || wmint->loop != mint->loop) {
            errorMsgNode(at, ErrorBrandLoop,
                "This value's brand was minted at %u:%u, in a pass of a loop, and its arena dies with that pass: it may not be kept where a later pass, or the code after the loop, could use it (wanted: %s).",
                mint->at->linenbr, (uint32_t)(mint->at->srcp - mint->at->linep) + 1,
                lifeBrandSpell(want, wantbuf, sizeof(wantbuf)));
            return;
        }
    }
    if (closed) {
        errorMsgNode(at, ErrorBrandLoop,
            "This value's brand was minted in a pass of a loop that has ended, and its arena died with that pass.");
        return;
    }
    if (want == NULL || given == NULL) {
        errorMsgNode(at, ErrorBrand,
            "This value carries %s, and %s is wanted here: an invariant lifetime is neither lost nor gained.",
            lifeBrandSpell(given, givenbuf, sizeof(givenbuf)), lifeBrandSpell(want, wantbuf, sizeof(wantbuf)));
        return;
    }
    if (brandBind)
        errorMsgNode(at, ErrorBrand,
            "This value carries %s, and the call's other values carry %s: a key unlocks only the arena it came from, which carries the same invariant lifetime.",
            lifeBrandSpell(given, givenbuf, sizeof(givenbuf)), lifeBrandSpell(want, wantbuf, sizeof(wantbuf)));
    else
        errorMsgNode(at, ErrorBrand,
            "This value carries %s, and %s is wanted here: one brand never stands for another.",
            lifeBrandSpell(given, givenbuf, sizeof(givenbuf)), lifeBrandSpell(want, wantbuf, sizeof(wantbuf)));
}

int lifeBrandsCoerce(INode *fromtype, INode *totype, INode *at) {
    if (!lifeInvariantSeen || fromtype == NULL || totype == NULL
        || totype == unknownType || totype == noCareType || fromtype == unknownType
        || itypeGetTypeDcl(fromtype) == errorType || itypeGetTypeDcl(totype) == errorType)
        return 1;
    LifeBrands given, want;
    lifeBrandsInit(&given);
    lifeBrandsInit(&want);
    lifeBrandsOf(fromtype, &given, 0);
    lifeBrandsOf(totype, &want, 0);
    if (given.cnt == 0 && want.cnt == 0)
        return 1;
    if (given.cnt != want.cnt) {
        lifeBrandError(at, want.cnt ? want.names[0] : NULL, given.cnt ? given.names[0] : NULL);
        return 0;
    }
    for (uint32_t i = 0; i < want.cnt; ++i) {
        if (!lifeBrandMatch(brandBind, want.names[i], given.names[i])) {
            // Under a binding, what is wanted is the brand bound already
            Name *bound = brandBind && want.names[i]
                ? lifePairFind(brandBind->from, brandBind->to, brandBind->cnt, want.names[i]) : NULL;
            lifeBrandError(at, bound ? bound : want.names[i], given.names[i]);
            return 0;
        }
    }
    return 1;
}

void lifeBindArgs(LifeBind *bind, Nodes *instargs, Nodes *written, INode *at) {
    if (!lifeInvariantSeen || instargs == NULL || written == NULL || instargs->used != written->used)
        return;
    LifeBrands decl, given;
    lifeBrandsInit(&decl);
    lifeBrandsInit(&given);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(instargs, cnt, nodesp))
        lifeBrandsOf(*nodesp, &decl, 0);
    for (nodesFor(written, cnt, nodesp))
        lifeBrandsOf(*nodesp, &given, 0);
    if (decl.cnt != given.cnt)
        return;
    for (uint32_t i = 0; i < decl.cnt; ++i) {
        if (decl.names[i] == NULL || given.names[i] == NULL)
            continue;
        if (!lifeBrandMatch(bind, decl.names[i], given.names[i])) {
            lifeBrandError(at, lifePairFind(bind->from, bind->to, bind->cnt, decl.names[i]), given.names[i]);
            return;
        }
    }
}

void lifeBindUse(LifeBind *bind, INode *strnode, INode *use, INode *at) {
    if (!lifeInvariantSeen || use == NULL || use == strnode || strnode->tag != StructTag)
        return;
    LifeBrands decl, given;
    lifeBrandsInit(&decl);
    lifeBrandsInit(&given);
    lifeBrandsOfStruct((StructNode *)strnode, NULL, &decl, 0);
    lifeBrandsOf(use, &given, 0);
    if (decl.cnt != given.cnt)
        return;
    for (uint32_t i = 0; i < decl.cnt; ++i) {
        if (decl.names[i] == NULL || given.names[i] == NULL)
            continue;
        if (!lifeBrandMatch(bind, decl.names[i], given.names[i])) {
            lifeBrandError(at, lifePairFind(bind->from, bind->to, bind->cnt, decl.names[i]), given.names[i]);
            return;
        }
    }
}

void lifeBindClose(LifeBind *bind, INode *at) {
    LifeOrder *order = bind->sig ? bind->sig->lifeorder : NULL;
    if (order == NULL)
        return;
    for (uint32_t i = 0; i < order->count; ++i) {
        Name *a = order->pairs[2 * i];
        Name *b = order->pairs[2 * i + 1];
        if (!lifeIsInvariant(a) || !lifeIsInvariant(b) || a == b)
            continue;
        Name *ba = lifePairFind(bind->from, bind->to, bind->cnt, a);
        Name *bb = lifePairFind(bind->from, bind->to, bind->cnt, b);
        if (ba && bb) {
            if (!lifeBrandAgree(ba, bb)) {
                char abuf[96], bbuf[96];
                errorMsgNode(at, ErrorBrand,
                    "This call's 'where %s == %s' wants one brand, and it is given two: %s and %s.",
                    &a->namestr, &b->namestr, lifeBrandSpell(ba, abuf, sizeof(abuf)), lifeBrandSpell(bb, bbuf, sizeof(bbuf)));
                return;
            }
        }
        else if (ba)
            lifePairAdd(&bind->from, &bind->to, &bind->cnt, &bind->cap, b, ba);
        else if (bb)
            lifePairAdd(&bind->from, &bind->to, &bind->cnt, &bind->cap, a, bb);
    }
}

// *********************
// Substitution
// *********************

// The brand a binding gives 'name': the one bound, else a fresh one minted
// at 'mintat' (and bound), else the unknown brand
static Name *lifeCanonName(uint32_t n);
static Name *lifeBrandMap(LifeBind *bind, Name *name, INode *mintat) {
    if (!lifeIsInvariant(name))
        return name;
    if (bind->positional)
        return lifeCanonName(++bind->seq);
    Name *to = lifePairFind(bind->from, bind->to, bind->cnt, name);
    if (to)
        return to;
    to = mintat ? lifeBrandMint(name, mintat) : unknownBrandName;
    lifePairAdd(&bind->from, &bind->to, &bind->cnt, &bind->cap, name, to);
    return to;
}

static INode *lifeBrandSubstAt(INode *type, LifeBind *bind, INode *mintat, int depth);

static INode *lifeBrandSubstStruct(StructNode *strnode, NameUseNode *use, LifeBind *bind, INode *mintat, int depth) {
    LifeParms *parms = strnode->lifeparms;
    int changed = 0;
    Name **names = NULL;
    uint32_t count = 0;
    if (parms && parms->count) {
        count = parms->count;
        names = memAllocBlk(count * sizeof(Name *));
        for (uint32_t k = 0; k < count; ++k) {
            Name *cur = use ? lifeSlotName((INode *)use, k) : parms->names[k];
            names[k] = cur;
            if (lifeIsInvariant(parms->names[k]) && cur) {
                names[k] = lifeBrandMap(bind, cur, mintat);
                if (names[k] != cur)
                    changed = 1;
            }
        }
    }
    Nodes *written = use && use->lifeuse ? use->lifeuse->typeargs : NULL;
    Nodes *typeargs = written ? written : itypeInstanceTypeArgs((INode *)strnode);
    Nodes *newargs = NULL;
    if (typeargs) {
        INode **nodesp;
        uint32_t cnt;
        int argchanged = 0;
        Nodes *substargs = newNodes(typeargs->used);
        for (nodesFor(typeargs, cnt, nodesp)) {
            INode *subst = lifeBrandSubstAt(*nodesp, bind, mintat, depth + 1);
            if (subst != *nodesp)
                argchanged = 1;
            nodesAdd(&substargs, subst);
        }
        if (argchanged) {
            newargs = substargs;
            changed = 1;
        }
    }
    // A declaration names no brands of its own outside its own methods: the
    // copy is always a use naming those bound
    if (use == NULL && (names || typeargs))
        changed = 1;
    if (!changed)
        return use ? (INode *)use : (INode *)strnode;
    NameUseNode *copy = (NameUseNode *)newNameUseFromDclNode((INode *)strnode, use ? (INode *)use : (mintat ? mintat : (INode *)strnode));
    LifeUse *lifeuse = memAllocBlk(sizeof(LifeUse));
    lifeuse->names = names;
    lifeuse->count = (uint16_t)count;
    lifeuse->typeargs = newargs ? newargs : written;
    lifeuse->at = use && use->lifeuse ? use->lifeuse->at : NULL;
    lifeuse->held = use && use->lifeuse ? use->lifeuse->held : NULL;
    copy->lifeuse = lifeuse;
    return (INode *)copy;
}

static INode *lifeBrandSubstAt(INode *type, LifeBind *bind, INode *mintat, int depth) {
    if (type == NULL || depth > 16)
        return type;
    switch (type->tag) {
    case NameUseTag:
    {
        if (!isTypeNode(type))
            return type;
        INode *dcl = itypeGetTypeDcl(type);
        if (dcl == NULL || dcl->tag != StructTag)
            return type;
        return lifeBrandSubstStruct((StructNode *)dcl, (NameUseNode *)type, bind, mintat, depth);
    }
    case StructTag:
        return lifeBrandSubstStruct((StructNode *)type, NULL, bind, mintat, depth);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        Name *life = lifeBrandMap(bind, ref->lifename, mintat);
        INode *vtexp = ref->vtexp && isTypeNode(ref->vtexp) ? lifeBrandSubstAt(ref->vtexp, bind, mintat, depth + 1) : ref->vtexp;
        if (life == ref->lifename && vtexp == ref->vtexp)
            return type;
        RefNode *copy = memAllocBlk(sizeof(RefNode));
        memcpy(copy, ref, sizeof(RefNode));
        copy->lifename = life;
        copy->vtexp = vtexp;
        // A key's lifetime is no scope: it goes anywhere
        if (lifeIsInvariant(life))
            copy->scope = 0;
        return (INode *)copy;
    }
    case PtrTag:
    {
        // A raw pointer carries no brand (lifeBrandsOf), so it holds no place
        if (bind->positional)
            return type;
        StarNode *ptr = (StarNode *)type;
        INode *vtexp = lifeBrandSubstAt(ptr->vtexp, bind, mintat, depth + 1);
        if (vtexp == ptr->vtexp)
            return type;
        StarNode *copy = memAllocBlk(sizeof(StarNode));
        memcpy(copy, ptr, sizeof(StarNode));
        copy->vtexp = vtexp;
        return (INode *)copy;
    }
    case TTupleTag:
    {
        TupleNode *tuple = (TupleNode *)type;
        INode **nodesp;
        uint32_t cnt;
        int changed = 0;
        Nodes *elems = newNodes(tuple->elems->used);
        for (nodesFor(tuple->elems, cnt, nodesp)) {
            INode *subst = lifeBrandSubstAt(*nodesp, bind, mintat, depth + 1);
            if (subst != *nodesp)
                changed = 1;
            nodesAdd(&elems, subst);
        }
        if (!changed)
            return type;
        TupleNode *copy = memAllocBlk(sizeof(TupleNode));
        memcpy(copy, tuple, sizeof(TupleNode));
        copy->elems = elems;
        return (INode *)copy;
    }
    default:
        return type;
    }
}

INode *lifeBrandSubst(INode *type, LifeBind *bind, INode *mintat) {
    if (!lifeInvariantSeen || bind == NULL)
        return type;
    return lifeBrandSubstAt(type, bind, mintat, 0);
}

INode *lifeBrandField(INode *objtype, INode *fldtype, INode *at) {
    if (!lifeInvariantSeen || objtype == NULL || fldtype == NULL || !lifeTypeHasBrands(fldtype))
        return fldtype;
    INode *dcl = itypeGetTypeDcl(objtype);
    if (dcl == NULL || dcl->tag != StructTag)
        return fldtype;
    // The struct as declared against the use read: a name of the struct's
    // (or an instance's ''=1') bound to the brand the use gives it, and to the
    // unknown brand where the use gives it none, or two
    LifeBrands decl, use;
    lifeBrandsInit(&decl);
    lifeBrandsInit(&use);
    lifeBrandsOfStruct((StructNode *)dcl, NULL, &decl, 0);
    lifeBrandsOf(objtype, &use, 0);
    LifeBind bind;
    lifeBindInit(&bind);
    if (decl.cnt == use.cnt) {
        for (uint32_t i = 0; i < decl.cnt; ++i) {
            Name *name = decl.names[i];
            if (name == NULL)
                continue;
            Name *given = use.names[i] ? lifeBrandResolve(use.names[i]) : unknownBrandName;
            Name *bound = lifePairFind(bind.from, bind.to, bind.cnt, name);
            if (bound == NULL)
                lifePairAdd(&bind.from, &bind.to, &bind.cnt, &bind.cap, name, given);
            else if (bound != given) {
                for (uint32_t b = 0; b < bind.cnt; ++b) {
                    if (bind.from[b] == name)
                        bind.to[b] = unknownBrandName;
                }
            }
        }
    }
    return lifeBrandSubstAt(fldtype, &bind, NULL, 0);
}

static int lifeKeyBorrowAt(INode *type, int depth) {
    if (type == NULL || depth > 16)
        return 0;
    switch (type->tag) {
    case NameUseTag:
    {
        LifeUse *lifeuse = ((NameUseNode *)type)->lifeuse;
        if (lifeuse && lifeuse->typeargs) {
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(lifeuse->typeargs, cnt, nodesp)) {
                if (lifeKeyBorrowAt(*nodesp, depth + 1))
                    return 1;
            }
        }
        return 0;
    }
    case RefTag:
    {
        RefNode *ref = (RefNode *)type;
        if (lifeIsInvariant(ref->lifename) && itypeCarriesBorrow(ref->vtexp))
            return 1;
        return isTypeNode(ref->vtexp) && lifeKeyBorrowAt(ref->vtexp, depth + 1);
    }
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (lifeKeyBorrowAt(*nodesp, depth + 1))
                return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

int lifeKeyBorrow(INode *type, INode *at) {
    if (!lifeInvariantSeen || !lifeKeyBorrowAt(type, 0))
        return 0;
    errorMsgNode(at, ErrorKeyBorrow,
        "This gives a key to a value that holds a borrow: what a key names lives in its arena, which outlives every scope, so it may hold no borrow.");
    return 1;
}

// Is this a brand an instance names its arguments' brands by, ''=1', ''=2'?
static int lifeIsCanon(Name *name) {
    return lifeIsInvariant(name) && name->namesz > 2 && isdigit((unsigned char)(&name->namestr)[2]);
}

static Name *lifeCanonName(uint32_t n) {
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "'=%u", n);
    return nametblFind(buf, (size_t)len);
}

// Each brand an argument carries is named by its place alone, the first
// ''=1', the next ''=2', whether or not two places carry one brand: the
// instance is then the same for every use of its shape, and assumes no two
// places alike; a call binds each place afresh, and a use whose places carry
// one brand binds them to it (lifeBindBegin), so nothing is lost by that.
void lifeCanonBrands(Nodes *args) {
    if (!lifeInvariantSeen || args == NULL)
        return;
    LifeBind map;
    lifeBindInit(&map);
    map.positional = 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(args, cnt, nodesp)) {
        if (isTypeNode(*nodesp))
            *nodesp = lifeBrandSubstAt(*nodesp, &map, NULL, 0);
    }
}

int lifeBrandsEqual(INode *a, INode *b) {
    if (!lifeInvariantSeen)
        return 1;
    LifeBrands ba, bb;
    lifeBrandsInit(&ba);
    lifeBrandsInit(&bb);
    lifeBrandsOf(a, &ba, 0);
    lifeBrandsOf(b, &bb, 0);
    if (ba.cnt != bb.cnt)
        return 0;
    for (uint32_t i = 0; i < ba.cnt; ++i) {
        if (ba.names[i] != bb.names[i])
            return 0;
    }
    return 1;
}

int lifeBrandKnown(INode *type, INode *at) {
    if (!lifeInvariantSeen)
        return 1;
    LifeBrands brands;
    lifeBrandsInit(&brands);
    lifeBrandsOf(type, &brands, 0);
    if (brands.cnt == 0)
        return 1;
    LifeBrands result;
    lifeBrandsInit(&result);
    if (brandFn)
        lifeBrandsOf(brandFn->sig->rettype, &result, 0);
    for (uint32_t i = 0; i < brands.cnt; ++i) {
        Name *name = brands.names[i];
        if (name == NULL || lifeIsCanon(name) || lifeMintOf(name))
            continue;
        if (brandFn && (lifeBrandsHas(&brandFn->parmnames, name) || lifeBrandsHas(&result, name)))
            continue;
        errorMsgNode(at, ErrorLifetimeInvariant,
            "'%s' is named by none of the function's parameters or its result: a body names only the invariant lifetimes its signature does.",
            &name->namestr);
        return 0;
    }
    return 1;
}

void lifeBrandBreaks(INode *blk, Nodes *breaks) {
    if (!lifeInvariantSeen || breaks == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(breaks, cnt, nodesp)) {
        INode *exp = ((BreakRetNode *)*nodesp)->exp;
        if (exp == NULL || !isExpNode(exp))
            continue;
        LifeBrands brands;
        lifeBrandsInit(&brands);
        lifeBrandsOf(((IExpNode *)exp)->vtype, &brands, 0);
        for (uint32_t i = 0; i < brands.cnt; ++i) {
            Name *name = lifeBrandResolve(brands.names[i]);
            LifeMint *mint = lifeMintOf(name);
            if (name && lifeBrandClosedLoop(name)) {
                errorMsgNode(exp, ErrorBrandLoop,
                    "This value's brand was minted at %u:%u, in a pass of a loop this 'break' leaves, and its arena dies with that pass: it may not be carried out of the loop.",
                    mint->at->linenbr, (uint32_t)(mint->at->srcp - mint->at->linep) + 1);
                break;
            }
        }
    }
}
