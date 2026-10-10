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
        // A generator's struct, and its members, are made again by every object
        // that uses the generator, from the text of its declaration: they are not
        // the package's to export, as a generic's instance is not
        if (node->tag == StructTag && yieldAny() && yieldGenOfStruct(node))
            return 1;
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
//
// 'gens' says whether a public generator counts as such a body. It does for what
// a library exports (typeHoldsExpanded); it does not in deciding which private
// generators an importer needs (yieldGenExpanded), where one public generator
// would otherwise make every other one in its module needed
static int typeHolds(INode *type, int gens) {
    if (type->tag != StructTag)
        return 0;
    StructNode *strnode = (StructNode*)type;
    // A generator's struct holds the inline function that makes what a 'yield'
    // hands out, which no importer calls: what it holds is the generator's own
    // affair (yieldGenExpanded)
    if (yieldAny() && yieldGenOfStruct(type))
        return 0;
    if (strnode->genericinfo || (type->flags & TraitType))
        return 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->nodelist, cnt, nodesp)) {
        INode *node = *nodesp;
        // A public generator is a body an importer expands. A private one is
        // not, unless the type holds another body that can reach it
        if (node->tag == MacroDclTag
            || (node->tag == FnDclTag && ((node->flags & FlagInline) || ((FnDclNode*)node)->genericinfo
                || (gens && (node->flags & FlagPub) && yieldAny() && yieldGenOfCtor((FnDclNode*)node)))))
            return 1;
    }
    return 0;
}

int typeHoldsExpanded(INode *type) {
    return typeHolds(type, 1);
}

// ---- Which generators an importer needs ---------------------------------------
//
// A generator is expanded -- carried whole in the include file, its body's names
// exported and declared beside it -- only where an importer can reach it:
// - a public one;
// - one named, by name resolution, in a body an importer expands (DclExpandReached);
// - a method of a type, when the type holds a body an importer expands (a public
//   generator among them), or an expanded body names the type, since a body can
//   call a private method through a value, which name resolution does not see;
//   or when the module holds an inline, generic or macro body, which can reach
//   any type's private methods the same way. A public generator in the module
//   does not count for that: one that reaches a private generator method of a
//   type it does not name is not seen.
// Any other generator, a private one nothing reaches, stays inside the package.

static int typeTreeHolds(INode *type, int gens);

// Whether a module holds a body an importer expands, not counting generators.
// Asked afresh each time: it is asked while the reaches are still being worked
// out, when a kept answer could be too early
static int modHoldsExpandedNoGens(ModuleNode *mod) {
    if (mod == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        INode *node = *nodesp;
        if (node->tag == FnDclTag) {
            FnDclNode *fn = (FnDclNode*)node;
            if ((fn->flags & FlagInline) || fn->genericinfo || (fn->dclinfo.facts & DclIntrinsic))
                return 1;
        }
        else if (node->tag == StructTag && typeTreeHolds(node, 0))
            return 1;
    }
    return 0;
}

// Whether the generator whose constructor is 'ctor' is expanded
int yieldGenExpanded(FnDclNode *ctor) {
    if ((ctor->flags & FlagPub) || (ctor->dclinfo.facts & DclExpandReached))
        return 1;
    INode *owner = ctor->dclinfo.owner;
    if (owner == NULL || owner->tag != StructTag)
        return 0;
    return (owner->tag == StructTag && (inodeGetDclInfo(owner)->facts & DclExpandReached))
        || typeHolds(owner, 1) || modHoldsExpandedNoGens(dclInfoGetModule(owner));
}

// The generators whose bodies name things an importer needs only if the
// generator is itself expanded, which name resolution cannot know when it reads
// the body (a callee's body may be read before its caller's): the bodies are
// resolved with their reaches recorded and not yet marked, and exportGenReach
// marks them once everything is resolved
static Nodes *condsteps = NULL;

void exportCondStep(FnDclNode *step) {
    if (condsteps == NULL)
        condsteps = newNodes(8);
    nodesAdd(&condsteps, (INode*)step);
}

int exportIsCondStep(INode *from) {
    if (condsteps == NULL || from == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(condsteps, cnt, nodesp)) {
        if (*nodesp == from)
            return 1;
    }
    return 0;
}

// Mark what the bodies of the expanded generators name, and so on: marking a
// private generator reached makes it expanded, and its body's names are marked in
// turn
void exportGenReach() {
    if (condsteps == NULL)
        return;
    int changed = 1;
    while (changed) {
        changed = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(condsteps, cnt, nodesp)) {
            FnDclNode *step = (FnDclNode*)*nodesp;
            GenInfo *info = yieldGenOf(step);
            if (info == NULL || info->ctor == NULL || !yieldGenExpanded(info->ctor))
                continue;
            Nodes *reached = exportReachesOf((INode*)step);
            if (reached == NULL)
                continue;
            INode **rp;
            uint32_t rcnt;
            for (nodesFor(reached, rcnt, rp)) {
                INode *to = *rp;
                if (to->tag != FnDclTag && to->tag != VarDclTag && to->tag != StructTag)
                    continue;
                DclInfo *toinfo = inodeGetDclInfo(to);
                if (toinfo == NULL || toinfo->owner == NULL || (toinfo->facts & DclExpandReached))
                    continue;
                toinfo->facts |= DclExpandReached;
                changed = 1;
            }
        }
    }
}

// Whether a module declares a body an importer expands anywhere in it: a
// function fnDclIsExpanded says is expanded, at module level or in a type, or a
// type typeHoldsExpanded says holds one, a variant inside an enum's braces
// included. A type's private members are its module's, so such a body can call
// a private method of any type of the module through a value, which name
// resolution cannot see. Asked once a module, and kept.
static int typeTreeHolds(INode *type, int gens) {
    if (typeHolds(type, gens))
        return 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&((StructNode*)type)->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == StructTag && typeTreeHolds(*nodesp, gens))
            return 1;
    }
    return 0;
}

static int typeTreeHoldsExpanded(INode *type) {
    return typeTreeHolds(type, 1);
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
    // A generator is carried whole or not at all (a bodiless one cannot be read
    // from the file): one no importer can reach stays inside the package
    if (dclnode->tag == FnDclTag && yieldAny() && yieldGenOfCtor((FnDclNode*)dclnode)
        && !yieldGenExpanded((FnDclNode*)dclnode))
        return 0;
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
