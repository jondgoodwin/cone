/** Named lifetimes: on a function's signature, on a struct, and ordered
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>

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

INode *lifeErased(INode *type) {
    if (type == NULL)
        return type;
    switch (type->tag) {
    case NameUseTag:
    {
        if (((NameUseNode *)type)->lifeuse == NULL)
            return type;
        NameUseNode *copy = memAllocBlk(sizeof(NameUseNode));
        memcpy(copy, type, sizeof(NameUseNode));
        copy->lifeuse = NULL;
        return (INode *)copy;
    }
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        INode *vtexp = lifeErased(ref->vtexp);
        if (ref->lifename == NULL && vtexp == ref->vtexp)
            return type;
        RefNode *copy = memAllocBlk(sizeof(RefNode));
        memcpy(copy, ref, sizeof(RefNode));
        copy->lifename = NULL;
        copy->vtexp = vtexp;
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
// unnamed lifetime is ordered against nothing; ''static' outlasts every one.
static int lifeOrdered(LifeOrder *order, Name *longer, Name *shorter) {
    if (longer == shorter || longer == staticLifeName)
        return 1;
    if (order == NULL || longer == NULL || shorter == NULL || shorter == staticLifeName)
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
    // ''static' ties a value to no caller lifetime
    if (name == staticLifeName)
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
        && itypeGetTypeDcl(((RefNode *)typedcl)->region) == borrowRef;
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
    if (dcl->tag != StructTag) {
        if (dcl != (INode *)use)
            lifeGather(dcl, anon, set);
        return;
    }
    LifeUse *lifeuse = use->lifeuse;
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
            if (lifeIsFnBorrow(ref))
                return;
            lifeSetAdd(set, anon ? NULL : ref->lifename);
        }
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

void lifeUseInstance(NameUseNode *instuse, INode *genuse, Nodes *typeargs) {
    LifeUse *genlife = genuse && genuse->tag == NameUseTag ? ((NameUseNode *)genuse)->lifeuse : NULL;
    int keep = typeargs && lifeArgsHoldBorrow(typeargs);
    if (genlife == NULL && !keep)
        return;
    LifeUse *lifeuse = memAllocBlk(sizeof(LifeUse));
    lifeuse->names = genlife ? genlife->names : NULL;
    lifeuse->count = genlife ? genlife->count : 0;
    lifeuse->at = genlife ? genlife->at : NULL;
    lifeuse->typeargs = keep ? typeargs : NULL;
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
}

// *********************
// Signatures
// *********************

// Add to 'order' what a struct a type uses orders, in the names the use gives
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
                if (l && s && l != s) {
                    if (*order == NULL)
                        *order = newLifeOrder();
                    lifeOrderAdd(*order, l, s, type);
                }
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
        lifeImplied(((RefNode *)type)->vtexp, order);
        return;
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

void lifeSigCheck(FnSigNode *sig) {
    if (sig->lifechecked)
        return;
    sig->lifechecked = 1;
    INode **nodesp;
    uint32_t cnt;
    LifeSet named;
    lifeSetInit(&named);
    for (nodesFor(sig->parms, cnt, nodesp))
        lifeGather(((IExpNode *)*nodesp)->vtype, 0, &named);
    lifeGather(sig->rettype, 0, &named);
    if (named.cnt > 0)
        sig->lifenamed = 1;
    // A 'where' clause orders the signature's own lifetimes
    LifeOrder *written = sig->lifeorder;
    if (written) {
        for (uint32_t i = 0; i < written->count; ++i) {
            for (int side = 0; side < 2; ++side) {
                Name *name = written->pairs[2 * i + side];
                if (!lifeSetHas(&named, name))
                    errorMsgNode(written->at[i], ErrorLifetimeUndeclared,
                        "'%s' is named by none of the function's parameters or its result, so its 'where' clause cannot order it.",
                        &name->namestr);
            }
        }
    }
    // A struct's order holds of each use of it: a value of 'Pair['x, 'y]' can
    // exist only where ''x' lasts at least as long as ''y' does
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

// Append, for a parameter, whether a value of the type 'to' may hold each of
// its parts: one digit for any, and, where 'detail' asks and it has several,
// one per part
static char *lifePromiseParts(char *p, FnSigNode *sig, INode *parm, INode *to, int anon, int extra, int detail) {
    uint32_t parts[LifeMaxSlots + 2];
    uint32_t n = lifeParts(parm, parts);
    int any = lifeFlowsAs(sig, parm, to, anon);
    *p++ = (char)('0' + any + extra);
    if (detail && n > 1) {
        for (uint32_t i = 0; i < n; ++i)
            *p++ = (char)('0' + lifePartFlowsAs(sig, parm, parts[i], to, anon));
    }
    return p;
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
// same agree.
static char *lifeSigPromises(FnSigNode *sig, int anon, int detail) {
    uint32_t nparms = sig->parms->used;
    char *promises = memAllocStr(NULL, (nparms + 1) * (nparms + 1) * (LifeMaxSlots + 3) + 1);
    char *p = promises;
    for (uint32_t i = 0; i < nparms; ++i) {
        INode *parmtype = ((IExpNode *)nodesGet(sig->parms, i))->vtype;
        int stores = lifeParmStores(parmtype);
        p = lifePromiseParts(p, sig, parmtype, sig->rettype, anon,
            (!anon && lifeIsStatic(parmtype) ? 2 : 0) + (stores ? 4 : 0), detail);
        if (!stores)
            continue;
        INode *pointee = lifePointee(parmtype);
        for (uint32_t k = 0; k < nparms; ++k) {
            if (k != i)
                p = lifePromiseParts(p, sig, ((IExpNode *)nodesGet(sig->parms, k))->vtype, pointee, anon, 0, detail);
        }
        if (detail && lifeSlotted(pointee))
            *p++ = (char)('0' + (anon ? lifeSlotsApart(NULL, pointee) : lifeSlotsApart(sig, pointee)));
    }
    *p = '\0';
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
int lifeSigsAgree(FnSigNode *a, FnSigNode *b) {
    if (!a->lifenamed && !b->lifenamed)
        return 1;
    if (a->parms->used != b->parms->used)
        return 0;
    int detail = strcmp(lifeSigShape(a), lifeSigShape(b)) == 0;
    return strcmp(lifeSigPromises(a, !a->lifenamed, detail), lifeSigPromises(b, !b->lifenamed, detail)) == 0;
}

char *lifeSigSpell(char *bufp, FnSigNode *sig) {
    if (!sig->lifenamed)
        return bufp;
    char *promises = lifeSigPromises(sig, 0, 1);
    if (strcmp(promises, lifeSigPromises(sig, 1, 1)) == 0)
        return bufp;
    *bufp++ = 'G';
    size_t len = strlen(promises);
    memcpy(bufp, promises, len);
    bufp += len;
    *bufp++ = '_';
    return bufp;
}
