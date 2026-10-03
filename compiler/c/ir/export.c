/** What a library compile exports
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "export.h"

#include <string.h>

// Whether a declaration belongs to a generic's instance: it is one (a generic
// function's instance), or it is a member of one (a method or static of a
// generic type's instance, or of an instance of a generic trait or enum). The
// members of a type's instance carry no instantiating node of their own; their
// type does, so the owners are asked up to the module -- and the module too,
// since every declaration of a generic module's instance is a member of it.
int dclIsInstance(INode *dclnode) {
    for (INode *node = dclnode; node; node = inodeGetOwner(node)) {
        if (node->tag == ModuleTag)
            return ((ModuleNode*)node)->generic != NULL;
        if (itypeInstanceTypeArgs(node) != NULL)
            return 1;
    }
    return 0;
}

// Whether a type's own braces hold a body an importer expands: an inline or
// generic method, a macro method, or -- in a trait or a generic type -- every
// method (fnDclIsExpanded). A private method is reached through a receiver,
// which name resolution cannot see (it binds the member at type check), so
// such a type exports all of them, as does a type whose module holds an
// expanded body elsewhere (modHoldsExpanded).
int typeHoldsExpanded(INode *type) {
    if (type->tag != StructTag)
        return 0;
    StructNode *strnode = (StructNode*)type;
    if (strnode->genericinfo || (type->flags & TraitType))
        return 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->nodelist, cnt, nodesp)) {
        INode *node = *nodesp;
        if (node->tag == MacroDclTag
            || (node->tag == FnDclTag && ((node->flags & FlagInline) || ((FnDclNode*)node)->genericinfo)))
            return 1;
    }
    return 0;
}

// Whether a module declares a body an importer expands anywhere in it: a
// function fnDclIsExpanded says is expanded, at module level or in a type, or a
// type typeHoldsExpanded says holds one, a variant inside an enum's braces
// included. A type's private members are its module's, so such a body can call
// a private method of any type of the module through a value, which name
// resolution cannot see. Asked once a module, and kept.
static int typeTreeHoldsExpanded(INode *type) {
    if (typeHoldsExpanded(type))
        return 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&((StructNode*)type)->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == StructTag && typeTreeHoldsExpanded(*nodesp))
            return 1;
    }
    return 0;
}

static int modHoldsExpanded(ModuleNode *mod) {
    if (mod == NULL)
        return 0;
    if (mod->holdsexpanded == 0) {
        mod->holdsexpanded = 1;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(mod->nodes, cnt, nodesp)) {
            INode *node = *nodesp;
            if ((node->tag == FnDclTag && fnDclIsExpanded((FnDclNode*)node, NULL))
                || (node->tag == StructTag && typeTreeHoldsExpanded(node))) {
                mod->holdsexpanded = 2;
                break;
            }
        }
    }
    return mod->holdsexpanded == 2;
}

// Whether a function is a type's 'final' or 'clone' -- by its own name or by the
// overload name it answers to -- or one of the methods the compiler calls on a
// region ref: 'alloc', 'init', 'aliasRef', 'dealiasRef', 'free', and a traced
// region's 'mark' and 'writeBarrier'. A value of the type calls the first two
// wherever it is dropped or copied, and a reference into the region calls the
// others wherever it is allocated, copied, dropped, traced or stored, which
// names none of them; and those it calls on a lock permission, 'init' where an
// allocation holds the lock and 'acquireMut', 'releaseMut', 'acquireRead' and
// 'releaseRead' wherever a borrow through a reference holding it begins and ends
int fnIsTypeLifecycle(INode *dclnode) {
    if (dclnode->tag != FnDclTag)
        return 0;
    FnDclNode *fn = (FnDclNode*)dclnode;
    INode *owner = fn->dclinfo.owner;
    if (owner == NULL || owner->tag != StructTag)
        return 0;
    if (fn->namesym == finalName || fn->namesym == cloneName
        || fn->overloadsym == finalName || fn->overloadsym == cloneName)
        return 1;
    Name *name = fn->namesym;
    if (structDeclaresTrait((StructNode*)owner, lockPermTrait)
        && (name == initMethodName || name == acquireMutMethodName || name == releaseMutMethodName
            || name == acquireReadMethodName || name == releaseReadMethodName))
        return 1;
    return regionIsRegionRef(owner)
        && (name == allocMethodName || name == initMethodName || name == aliasRefMethodName
            || name == dealiasRefMethodName || name == freeMethodName || name == markMethodName
            || name == writeBarrierMethodName);
}

// Whether a type's method meets a requirement, or takes the place of a default,
// of a trait the type is -- its base, or a further name of its 'is' list. Wherever a value of
// the type is coerced to the trait, a vtable is built that calls it, naming
// nothing: in an importer's object as in the package's. Each trait is asked of
// the type's 'traits', where taking its members in recorded it: by now no
// placeholder field is left to ask. A trait's own namespace holds what it took
// from the traits it is, so a requirement inherited that way is found too.
static int traitHasMember(INode *trait, Name *name) {
    if (trait == NULL || !isTypeNode(trait) || trait->tag == FnCallTag)
        return 0;
    trait = itypeGetTypeDcl(trait);
    return trait->tag == StructTag && namespaceFind(&((StructNode*)trait)->namespace, name) != NULL;
}

int fnIsTraitMethod(INode *dclnode) {
    if (dclnode->tag != FnDclTag)
        return 0;
    FnDclNode *fn = (FnDclNode*)dclnode;
    INode *owner = fn->dclinfo.owner;
    if (owner == NULL || owner->tag != StructTag || fn->namesym == NULL)
        return 0;
    StructNode *strnode = (StructNode*)owner;
    if (traitHasMember(strnode->basetrait, fn->namesym))
        return 1;
    if (strnode->traits == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(strnode->traits, cnt, nodesp)) {
        if (traitHasMember(*nodesp, fn->namesym))
            return 1;
    }
    return 0;
}

// Whether a library compile exports a definition, so that an importer's
// object links against it (names-and-namespaces.md, "Linkage", L1 and L5).
// Only the package's own modules export: the root and its submodules, never
// core or a package compiled in beside them. Of those, a definition is
// exported when it is:
// - named by a body an importer expands (DclExpandReached): an inline,
//   generic or macro body, a trait default, a generic type's method --
//   private or not; or
// - a module's 'init', its 'final' or the 'drop' it is given (DclLifecycle),
//   public or not, since the program's stitched init and final call them; or
// - a public function or global of a module; or
// - a type or function an 'actor' generated, or a function of such a type
//   (DclActorGen): the include file holds the actor whole, and an importer's
//   instances of the actors package's generics call them; or
// - a function of a type an importer can reach -- a public type, one an
//   expanded body names, or one the include file declares (DclIncluded) --
//   when the function is public, or the type, or any part of the module that
//   declares it, holds an expanded body that can reach its private ones
//   through a receiver (typeHoldsExpanded, modHoldsExpanded), or the
//   function is the type's 'final' or 'clone', which an importer's object
//   calls wherever it drops or copies a value of the type, or it meets a
//   trait's requirement, which a vtable an importer builds calls.
// Everything else is internal: a private definition nothing expanded names, and
// every function of a private type no expanded body names. An instance of a
// generic, or a member of one, is never exported: every object that uses it
// defines it.
int dclIsExported(ModuleNode *libroot, INode *dclnode) {
    if (libroot == NULL || dclIsInstance(dclnode))
        return 0;
    ModuleNode *mod = dclInfoGetModule(dclnode);
    while (mod && mod->dclinfo.owner)
        mod = dclInfoGetModule(mod->dclinfo.owner);
    if (mod != libroot)
        return 0;
    DclInfo *dclinfo = inodeGetDclInfo(dclnode);
    if (dclinfo->facts & (DclExpandReached | DclLifecycle | DclActorGen))
        return 1;
    INode *owner = dclinfo->owner;
    DclInfo *ownerinfo = owner && owner->tag != ModuleTag ? inodeGetDclInfo(owner) : NULL;
    if (ownerinfo && (ownerinfo->facts & DclActorGen))
        return 1;
    if (owner == NULL || owner->tag == ModuleTag)
        return !(dclinfo->facts & DclPrivate);
    // A type the include file declares (DclIncluded) is one an importer holds
    // values of, whether or not it can name it: a private type a public one
    // holds in a field, say, whose public methods the importer calls through
    // that field
    DclInfo *typeinfo = inodeGetDclInfo(owner);
    if (typeinfo == NULL
        || ((typeinfo->facts & DclPrivate) && !(typeinfo->facts & (DclExpandReached | DclIncluded))))
        return 0;
    return !(dclinfo->facts & DclPrivate) || typeHoldsExpanded(owner)
        || fnIsTypeLifecycle(dclnode) || fnIsTraitMethod(dclnode)
        || modHoldsExpanded(dclInfoGetModule(owner));
}

// ---- What expanded bodies reach ---------------------------------------------

// A table from each expanded body to the declarations it names, open
// addressing on the body's address
typedef struct ReachSlot {
    INode *from;
    Nodes *to;
} ReachSlot;

static ReachSlot *reachslots = NULL;
static size_t reachavail = 0, reachused = 0;

static size_t reachHash(INode *from, size_t avail) {
    size_t h = (size_t)from;
    h ^= h >> 17;
    h *= 0x9E3779B1u;
    return (h ^ (h >> 13)) & (avail - 1);
}

static ReachSlot *reachSlot(INode *from) {
    size_t i = reachHash(from, reachavail);
    while (reachslots[i].from != NULL && reachslots[i].from != from)
        i = (i + 1) & (reachavail - 1);
    return &reachslots[i];
}

void exportReachAdd(INode *from, INode *to) {
    if (from == NULL || to == NULL || from == to)
        return;
    if ((reachused + 1) * 2 > reachavail) {
        ReachSlot *old = reachslots;
        size_t oldavail = reachavail;
        reachavail = oldavail ? oldavail * 2 : 256;
        reachslots = (ReachSlot*)memAllocBlk(reachavail * sizeof(ReachSlot));
        memset(reachslots, 0, reachavail * sizeof(ReachSlot));
        for (size_t i = 0; i < oldavail; ++i) {
            if (old[i].from)
                *reachSlot(old[i].from) = old[i];
        }
    }
    ReachSlot *slot = reachSlot(from);
    if (slot->from == NULL) {
        slot->from = from;
        slot->to = newNodes(4);
        ++reachused;
    }
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(slot->to, cnt, nodesp)) {
        if (*nodesp == to)
            return;
    }
    nodesAdd(&slot->to, to);
}

Nodes *exportReachesOf(INode *from) {
    if (reachavail == 0 || from == NULL)
        return NULL;
    ReachSlot *slot = reachSlot(from);
    return slot->from ? slot->to : NULL;
}
