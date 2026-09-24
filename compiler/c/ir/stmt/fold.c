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
// takes and 'but' does not leave out. Not Self, not an unnamed node, not a
// private name, and never the source's own finalizer or clone, which belong to
// its values' lifecycle. A module's star clause is made pass by pass instead,
// since a module read round a cycle of imports holds more in a later pass
// (importFoldStar).
void foldStarItems(Namespace *ns, Name *srcname, FoldClause *fold, int admit) {
    INode **nodesp;
    uint32_t cnt;
    if (fold->excludes) {
        for (nodesFor(fold->excludes, cnt, nodesp)) {
            Name *name = ((NameUseNode*)*nodesp)->namesym;
            INode *found = namespaceFind(ns, name);
            if (found == NULL)
                errorMsgNode(*nodesp, ErrorNoMbr, "%s has no member named %s to leave out.",
                    &srcname->namestr, &name->namestr);
            // Seen from the fold, a private name is as absent as a missing one:
            // the star never admits it, so leaving it out says nothing
            else if (inodeIsPrivate(found))
                errorMsgNode(*nodesp, ErrorNotPublic, "%s is private to %s, so '*' never folds it; there is nothing to leave out.",
                    &name->namestr, &srcname->namestr);
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
        // Nobody wrote this name: the clause asked for every name, so where it
        // meets another binding of the same declaration in a module, neither was
        // written twice and the two are one (modFoldBind)
        alias->flags |= FlagUnlisted;
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
    // 'pub use'. This is the binding carrying its own visibility: the member is
    // public in its type either way, and what 'pub' decides is whether the
    // MODULE shows the name it gave it.
    if (!global->fold->ispub)
        alias->flags &= 0xffff - FlagPub;
    // The same member through the same global, reached a second time by a
    // wildcard, is the binding the name has already, and listed twice it is
    // refused (modFoldBind); through another global it is another thing, and
    // collides
    INode *prior = modFoldBind(mod, alias);
    if (prior)
        modFoldDupReport(modFoldCollisionAt(mod, (INode*)global, (INode*)alias, prior), alias, prior);
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
            "A 'pub use' fold is reached from outside through %s, and %s is private. Declare it 'pub', or fold privately.",
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

// ---- A module's standalone 'use' -------------------------------------------
//
// 'use' standing as a statement names a namespace this module reaches WITHOUT
// an import, and folds its names in as names of the module [Jon 23 Sep]. Two
// kinds of namespace are reached that way:
//
// - An ENUM. Its variants are names of the enum, so 'Colors.Red' is the
//   spelling everywhere, the declaring module included, and 'use Colors;' is how
//   a module asks for them bare: each variant it admits becomes an alias in the
//   module's namespace, reached with no receiver -- the binding an import's fold
//   makes.
// - A SUBMODULE of this module, by its name or by a path through submodules:
//   'use scaling;', 'use geometry.scaling;'. Its public names fold exactly as an
//   import's clause folds a module's -- the same items, the same visibility rule,
//   the same passes -- because it IS that fold: an ImportNode marked 'isuse' over
//   the submodule, sharing the statement's clause (foldModUseModule).
//
// A module brought in by an import is not folded here: its import carries a
// clause of its own for that, and one situation has one spelling. Either fold is
// private to the module unless the statement says 'pub use'.

// Create a module's standalone 'use', positioned at its 'use'
ModUseNode *newModUseNode() {
    ModUseNode *node;
    newNode(node, ModUseNode, ModUseTag);
    node->source = NULL;
    node->fold = newFoldClause();
    node->modfold = NULL;
    return node;
}

// Serialize a module's standalone 'use'
void modUsePrint(ModUseNode *node) {
    inodeFprint(node->fold->ispub ? "pub use " : "use ");
    inodePrintNode(node->source);
}

// Is this entry of an enum's namespace one of its variants? The enum's own
// 'Self' is a struct too, and it is the enum.
static int foldIsVariant(StructNode *src, INode *member) {
    return member->tag == StructTag && member != (INode*)src;
}

// Enter one variant in the module's namespace, under its alias's spelling
static void foldEnumUseBind(ModuleNode *mod, ModUseNode *use, AliasDclNode *alias, INode *variant) {
    FoldClause *fold = use->fold;
    ((NameUseNode*)alias->target)->dclnode = variant;
    // Reached with no receiver, and as visible as the statement says: a fold is
    // private to the module that made it unless it is 'pub use'
    alias->flags &= 0xffff - (FlagPub | FlagMethFld);
    if (fold->ispub)
        alias->flags |= FlagPub;
    // 'use Colors;' names the enum, so every variant it binds is WRITTEN, even
    // where the statement lists none: a second 'use' of the same enum, or a
    // listed import of the same variant, is refused. The same variant brought
    // by a wildcard -- the core import's Some and None, most often -- is the
    // binding the name has already (modFoldBind)
    INode *prior = modFoldBind(mod, alias);
    if (prior)
        modFoldDupReport(modFoldCollisionAt(mod, (INode*)use, (INode*)alias, prior), alias, prior);
}

// Check a variant a clause names -- one it lists, or one its 'but' leaves out --
// returning it, or NULL once what is wrong with the name is reported
static INode *foldEnumUseVariant(StructNode *src, INode *at, Name *name, char *what) {
    INode *found = namespaceFind(&src->namespace, name);
    if (found == NULL) {
        errorMsgNode(at, ErrorNoMbr, "%s has no variant named %s to %s.",
            &src->namesym->namestr, &name->namestr, what);
        return NULL;
    }
    // A field or a method is reached through a value, and a static through the
    // enum; neither is a name the module could hold on its own
    if (!foldIsVariant(src, found)) {
        errorMsgNode(at, ErrorBadFold,
            "%s is a member of %s, not one of its variants. A module's 'use' folds in variants and nothing else of an enum.",
            &name->namestr, &src->namesym->namestr);
        return NULL;
    }
    return found;
}

// Collect the names a source is written with, before name resolution collapses
// a path into its last name. A path's names are the same nodes afterwards, each
// bound to what its step found, so this is what can still say which binding
// every step went through
static void foldModUseNames(INode *source, Nodes **names) {
    while (source && source->tag == FnCallTag) {
        FnCallNode *path = (FnCallNode*)source;
        if (path->methfld == NULL || !isNameUseNode(path->methfld) || path->args != NULL)
            return;
        nodesAdd(names, path->methfld);
        source = path->objfn;
    }
    if (source && isNameUseNode(source))
        nodesAdd(names, source);
}

// The step of a written source that an import reached -- the last such step,
// where there are several -- or NULL. A
// submodule is bound in its parent as the module itself; the name an import binds
// to its module is an alias, and so is every name a fold brought in, so a step
// bound to an alias is a step through an import
static NameUseNode *foldModUseImported(Nodes *names) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(names, cnt, nodesp)) {
        INode *binding = ((NameUseNode*)*nodesp)->dclnode;
        if (binding && binding->tag == AliasDclTag && !(binding->flags & FlagTypeAlias))
            return (NameUseNode*)*nodesp;
    }
    return NULL;
}

// Is 'inner' a submodule of 'mod', at any depth?
static int foldModWithin(ModuleNode *inner, ModuleNode *mod) {
    for (ModuleNode *up = (ModuleNode*)inner->dclinfo.owner; up; up = (ModuleNode*)up->dclinfo.owner) {
        if (up == mod)
            return 1;
    }
    return 0;
}

// Expand a standalone 'use' whose source is a module: refuse what is not reached
// without an import, then fold its public names in as an import's clause would.
//
// Only a submodule of this module is reached that way. A module an import
// reached -- named by the import's binding, or by a path through it -- is folded
// by the import's own clause, and the diagnostic says so (ErrorUseImported). Any
// other module is out of reach: this module itself, a module containing it, or
// one beside it, which only the fold passes' layered name table could have let
// the name reach (ErrorModReach).
//
// A submodule is folded once per module, as a module is imported once: a second
// 'use' of it is refused, naming the first.
static void foldModUseModule(NameResState *pstate, ModuleNode *mod, ModUseNode *use, Nodes *names, ModuleNode *src) {
    if (!foldModWithin(src, mod)) {
        NameUseNode *via = foldModUseImported(names);
        if (via != NULL && via == (NameUseNode*)use->source)
            errorMsgNode(use->source, ErrorUseImported,
                "%s is imported into this module, and an imported module's names are folded by its import's own clause: write 'import %s use ...'. A standalone 'use' folds what this module reaches without an import: an enum, or a submodule of its own.",
                &via->namesym->namestr, &via->namesym->namestr);
        else if (via != NULL)
            errorMsgNode(use->source, ErrorUseImported,
                "%s is reached through %s, which is imported into this module. A standalone 'use' folds what this module reaches without an import: an enum, or a submodule of its own.",
                &src->namesym->namestr, &via->namesym->namestr);
        else if (src == mod)
            errorMsgNode(use->source, ErrorModReach,
                "%s is this module. A standalone 'use' folds the names of a submodule of this one, and a module's own names are its already.",
                &src->namesym->namestr);
        else
            errorMsgNode(use->source, ErrorModReach,
                "Module %s is not a submodule of this one. A standalone 'use' folds the names of a module this one contains; a module elsewhere in the tree is reached by an import.",
                &src->namesym->namestr);
        return;
    }
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->moduses, cnt, nodesp)) {
        ModUseNode *prior = (ModUseNode*)*nodesp;
        if (prior == use)
            break;
        if (prior->modfold && prior->modfold->module == src) {
            errorMsgNode((INode*)use, ErrorDupImport,
                "Module %s is folded already, at %s:%u. A module folds a submodule's names once: write what both say in one 'use'.",
                &src->namesym->namestr, prior->lexer->url, prior->linenbr);
            return;
        }
    }
    ImportNode *modfold = newImportNode();
    inodeLexCopy((INode*)modfold, (INode*)use);
    modfold->module = src;
    modfold->isuse = 1;
    modfold->fold = use->fold;
    use->modfold = modfold;
    // Dependency-first, as for an import: what the submodule re-exports is in its
    // namespace once its own folds have run
    modFoldNames(pstate, src);
    importNameRes(pstate, modfold);
}

// Expand a module's standalone 'use': resolve the enum or submodule it names,
// then bind each name it admits in the module's namespace.
void foldModUseExpand(NameResState *pstate, ModuleNode *mod, ModUseNode *use) {
    FoldClause *fold = use->fold;
    // A submodule's fold runs in every pass, as an import's clause does: a star
    // clause reads the submodule afresh, and a listed name not yet there waits
    if (use->modfold) {
        modFoldNames(pstate, use->modfold->module);
        importNameRes(pstate, use->modfold);
        return;
    }
    if (fold->expanded)
        return;
    // An enum named through a binding not there yet -- a re-export still to
    // arrive round a cycle of imports -- waits for a later fold pass
    if (!modFoldReporting() && modFoldAwaits(use->source)) {
        modFoldWait();
        return;
    }
    fold->expanded = 1;

    // The enum or submodule is named as a type is, and a path is collapsed like
    // any other. Its steps are kept, for a module reached through an import
    Nodes *names = newNodes(4);
    foldModUseNames(use->source, &names);
    inodeNameRes(pstate, &use->source);
    // A path whose member did not resolve was reported where it is written, and
    // is left uncollapsed: it is not a name of the wrong kind
    if (use->source->tag == FnCallTag) {
        FnCallNode *path = (FnCallNode*)use->source;
        if (path->methfld && isNameUseNode(path->methfld)
            && ((NameUseNode*)path->methfld)->dclnode == NULL)
            return;
    }
    if (!isNameUseNode(use->source)) {
        errorMsgNode(use->source, ErrorUseEnum,
            "A module's 'use' names an enum declaration. A generic enum's variants are folded in by naming the enum alone, as in 'use Option;'.");
        return;
    }
    NameUseNode *srcname = (NameUseNode*)use->source;
    if (srcname->dclnode == NULL)
        return;     // reported where the name is written
    // A typedef's target is resolved with the module's other nodes, after every
    // fold, so what it names is not known here. Refused rather than resolved out
    // of turn: the enum is named by its own name.
    INode *binding = srcname->dclnode;
    while (binding && binding->tag == AliasDclTag && !(binding->flags & FlagTypeAlias))
        binding = ((NameUseNode*)((AliasDclNode*)binding)->target)->dclnode;
    if (binding && binding->tag == AliasDclTag) {
        errorMsgNode(use->source, ErrorUseEnum,
            "%s is a typedef. A module's 'use' names the enum declaration itself.",
            &srcname->namesym->namestr);
        return;
    }
    INode *dcl = nameUseGetDcl(srcname);
    if (dcl != NULL && dcl->tag == ModuleTag) {
        foldModUseModule(pstate, mod, use, names, (ModuleNode*)dcl);
        return;
    }
    if (dcl == NULL || dcl->tag != StructTag || !(dcl->flags & EnumType)) {
        errorMsgNode(use->source, ErrorUseEnum,
            "%s is not an enum or a module. A module's standalone 'use' folds in an enum's variants or a submodule's public names, and nothing else has names to fold.",
            &srcname->namesym->namestr);
        return;
    }
    StructNode *src = (StructNode*)dcl;

    // A 'pub' fold publishes the variants under this module's names, which may
    // not widen them past the enum they belong to
    if (fold->ispub && inodeIsPrivate((INode*)src)) {
        errorMsgNode(fold->at, ErrorNotPublic,
            "A 'pub use' makes the variants of %s public names of this module, and %s is private. Declare it 'pub', or fold privately.",
            &src->namesym->namestr, &src->namesym->namestr);
        return;
    }

    // An enum that extends another holds copies of its base's variants, made
    // while it is resolved, so it is resolved now: every one of its variants
    // folds, the copies with the ones it declared
    if (!structEnumDemandSet(pstate, src)) {
        errorMsgNode(use->source, ErrorCircular,
            "A 'use' needs the variants of %s, and its resolution is what reached this 'use'.",
            &src->namesym->namestr);
        return;
    }

    INode **nodesp;
    uint32_t cnt;
    if (fold->star) {
        if (fold->excludes) {
            for (nodesFor(fold->excludes, cnt, nodesp))
                foldEnumUseVariant(src, *nodesp, ((NameUseNode*)*nodesp)->namesym, "leave out");
        }
        // Only a variant another module may name folds from there. A variant is
        // as visible as its enum, and a private enum is not reached from outside
        // at all, so this passes nothing over unless something else is wrong.
        ModuleNode *srcmod = dclInfoGetModule((INode*)src);
        int crossmod = srcmod != NULL && srcmod != mod;
        namespaceFor(&src->namespace) {
            NameNode *nn = &src->namespace.namenodes[__i];
            if (nn->name == NULL || !foldIsVariant(src, nn->node) || foldExcluded(fold, nn->name))
                continue;
            if (crossmod && inodeIsPrivate(nn->node))
                continue;
            NameUseNode *target = newMemberUseNode(nn->name);
            inodeLexCopy((INode*)target, fold->at);
            AliasDclNode *alias = newNameAliasDclNode(nn->name, (INode*)target);
            inodeLexCopy((INode*)alias, fold->at);
            // Kept on the clause, as a listed item is, so a collision can tell
            // which fold made the binding (modFoldCollisionAt)
            nodesAdd(&fold->items, (INode*)alias);
            foldEnumUseBind(mod, use, alias, nn->node);
        }
        return;
    }
    for (nodesFor(fold->items, cnt, nodesp)) {
        AliasDclNode *alias = (AliasDclNode*)*nodesp;
        INode *variant = foldEnumUseVariant(src, (INode*)alias,
            ((NameUseNode*)alias->target)->namesym, "fold in");
        if (variant)
            foldEnumUseBind(mod, use, alias, variant);
    }
}
