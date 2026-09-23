/** Name folding at a namespace: the parts a type's fold and a module's share
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>

// The declaration of a type expression, through a reference or pointer if it is
// one, or NULL when it is not a declaration yet: an instance of a generic still
// to be instantiated, or a name that did not resolve.
INode *foldSourceDcl(INode *vtype) {
    if (vtype == NULL || vtype->tag == FnCallTag || !isTypeNode(vtype))
        return NULL;
    INode *dcl = itypeGetTypeDcl(vtype);
    if (dcl->tag == RefTag || dcl->tag == VirtRefTag)
        vtype = ((RefNode*)dcl)->vtexp;
    else if (dcl->tag == PtrTag)
        vtype = ((StarNode*)dcl)->vtexp;
    else
        return dcl;
    if (vtype->tag == FnCallTag || !isTypeNode(vtype))
        return NULL;
    return itypeGetTypeDcl(vtype);
}

// Is this name one a clause's 'but' leaves out?
int foldExcluded(FoldClause *fold, Name *name) {
    if (fold->excludes == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(fold->excludes, cnt, nodesp))
        if (((NameUseNode*)*nodesp)->namesym == name)
            return 1;
    return 0;
}

// Does the source namespace declare this member itself?
//
// Only what the sibling declares. A FIELD is the representation both types take
// from the base, so it is already a field here, and no type reads another type's
// fields. An ALIAS is what the sibling took from that same base, or delegated
// through a field of its own -- the first is here already by the same route, and
// the second is reached by naming the type it came from. What is left is the
// sibling's own contribution, which is the whole reason to name it.
int foldAdmitsOwn(INode *member) {
    return member->tag != FieldDclTag && member->tag != AliasDclTag;
}

// Does a star clause admit this member of the source namespace?
static int foldStarAdmits(INode *member, int admit) {
    if (admit == FoldAdmitOwn)
        return foldAdmitsOwn(member);
    // Every member reached through a value. A static is reached through the type,
    // so it is not one, and a star clause passes it over rather than refusing it
    return inodeIsMember(member);
}

// Make the items of a star clause: an alias for every name of 'ns' that 'admit'
// takes and 'but' does not leave out. Not Self, not an unnamed node, and never
// the source's own finalizer or clone, which belong to its values' lifecycle.
void foldStarItems(Namespace *ns, Name *srcname, FoldClause *fold, int admit) {
    INode **nodesp;
    uint32_t cnt;
    if (fold->excludes) {
        for (nodesFor(fold->excludes, cnt, nodesp)) {
            Name *name = ((NameUseNode*)*nodesp)->namesym;
            if (namespaceFind(ns, name) == NULL)
                errorMsgNode(*nodesp, ErrorNoMbr, "%s has no member named %s to leave out.",
                    &srcname->namestr, &name->namestr);
        }
    }
    namespaceFor(ns) {
        NameNode *nn = &ns->namenodes[__i];
        if (nn->name == NULL || nn->name == selfTypeName || nn->name == anonName
            || nn->name == finalName || nn->name == cloneName)
            continue;
        if (inodeIsPrivate(nn->node) || !foldStarAdmits(nn->node, admit) || foldExcluded(fold, nn->name))
            continue;
        NameUseNode *target = newMemberUseNode(nn->name);
        inodeLexCopy((INode*)target, fold->at);
        AliasDclNode *alias = newAliasDclNode(nn->name, (INode*)target);
        inodeLexCopy((INode*)alias, fold->at);
        nodesAdd(&fold->items, (INode*)alias);
    }
}

// ---- A global's fold clause ------------------------------------------------
//
// A global is the one-instance analogue of a field: 'config Config use *' in a
// module body admits Config's members as names of the module, reached through
// 'config'. Every entry is an alias, field and method alike, and 'through' is
// the global -- there is nothing to copy and no offset to carry, because the
// receiver is one address known at compile time. A use of the name is lowered to
// 'config.name' (nameUseTypeCheck, fnCallTypeCheck) and from there it is the
// path the author could have written, so nothing downstream is new.
//
// What it buys that folding a module cannot: a module composed from a STRUCT,
// presenting a singleton's interface as its own names.

// Expand one item of a global's fold clause: bind its target in the global's
// type and enter it in the module's namespace as an alias reached through the
// global.
static void foldGlobalItem(ModuleNode *mod, VarDclNode *global, StructNode *src, AliasDclNode *alias) {
    NameUseNode *target = (NameUseNode*)alias->target;
    Name *srcname = target->namesym;
    INode *found = namespaceFind(&src->namespace, srcname);
    if (found == NULL) {
        errorMsgNode((INode*)alias, ErrorNoMbr, "%s has no member named %s to fold in.",
            &src->namesym->namestr, &srcname->namestr);
        return;
    }
    // Ahead of the visibility check, because a lifecycle method does not fold
    // whether it is public or not
    if (srcname == finalName || srcname == cloneName) {
        errorMsgNode((INode*)alias, ErrorBadFold, "%s belongs to %s's own values' lifecycle, so it does not fold.",
            &srcname->namestr, &src->namesym->namestr);
        return;
    }
    // Visibility is transitive: only what the global's type shows folds
    if (inodeIsPrivate(found)) {
        errorMsgNode((INode*)alias, ErrorNotPublic, "%s is private to %s, so it does not fold.",
            &srcname->namestr, &src->namesym->namestr);
        return;
    }
    // Through the source's own aliases to the declaration; a source alias the
    // source's fold failed to bind was reported there
    INode *dcl = aliasDclResolve(found);
    if (dcl == NULL)
        return;
    // A static is a one-instance thing reached through the type, and a global is
    // the value route: there is no such thing as a static reached through a
    // value, whether the value is a field or a module's one instance
    if (!inodeIsMember(dcl)) {
        errorMsgNode((INode*)alias, ErrorBadFold,
            "%s is a static of %s: it is reached through the type, not through a value, so it does not fold. Name it as %s.%s.",
            &srcname->namestr, &src->namesym->namestr, &src->namesym->namestr, &srcname->namestr);
        return;
    }
    target->dclnode = dcl;
    alias->through = (INode*)global;
    // A fold is private to the namespace that made it unless the clause says
    // 'use pub'. This is the binding carrying its own visibility: the member is
    // public in its type either way, and what 'pub' decides is whether the
    // MODULE shows the name it gave it.
    if (!global->fold->ispub)
        alias->flags &= 0xffff - FlagPub;
    INode *prior = namespaceAdd(&mod->namespace, alias->namesym, (INode*)alias);
    if (prior) {
        errorMsgNode((INode*)alias, ErrorDupName,
            "%s is already a name of this module. A folded name must be unique: rename it with 'as', or leave it out with 'but'.",
            &alias->namesym->namestr);
        return;
    }
    nametblHookNode(alias->namesym, (INode*)alias);
}

// Expand a global's fold clause into its module's namespace. Run before the
// module's other nodes are name resolved, so that a folded name is in place
// wherever it is used, including above the global that folded it.
void foldGlobalExpand(NameResState *pstate, ModuleNode *mod, VarDclNode *global) {
    FoldClause *fold = global->fold;
    INode *srcdcl = foldSourceDcl(global->vtype);
    if (srcdcl == NULL)
        return;      // the type did not resolve, and that was reported where it is written
    fold->expanded = 1;
    if (srcdcl->tag != StructTag) {
        errorMsgNode(fold->at, ErrorUseGlobal,
            "A fold takes its names from a struct, and the type of %s is not one.",
            &global->namesym->namestr);
        return;
    }
    StructNode *src = (StructNode*)srcdcl;
    if (src->flags & EnumType) {
        errorMsgNode(fold->at, ErrorUseGlobal,
            "%s is an enum, and its variant set is its identity rather than a set of members to fold.",
            &src->namesym->namestr);
        return;
    }
    if (src->flags & TraitType) {
        errorMsgNode(fold->at, ErrorUseGlobal,
            "%s is a trait. A fold reaches through a value's own members, and an abstraction has none to reach.",
            &src->namesym->namestr);
        return;
    }
    // A 'pub' fold is reached from outside through the global, so the global must
    // be reachable from outside too. A private global folds privately, which is
    // the default and needs nothing of it.
    if (fold->ispub && inodeIsPrivate((INode*)global)) {
        errorMsgNode(fold->at, ErrorNotPublic,
            "A 'use pub' fold is reached from outside through %s, and %s is private. Declare it 'pub', or fold privately.",
            &global->namesym->namestr, &global->namesym->namestr);
        return;
    }
    // The type's own members must be in place before they are read, wherever its
    // module's own resolution has got to
    if (!structNameResDemand(pstate, src)) {
        errorMsgNode(fold->at, ErrorCircular,
            "A fold needs %s complete, and its resolution is what reached this fold.",
            &src->namesym->namestr);
        return;
    }
    if (fold->star)
        foldStarItems(&src->namespace, src->namesym, fold, FoldAdmitMembers);
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(fold->items, cnt, itemp))
        foldGlobalItem(mod, global, src, (AliasDclNode*)*itemp);
}
