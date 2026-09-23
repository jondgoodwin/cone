/** import node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new Import node
ImportNode *newImportNode() {
    ImportNode *node;
    newNode(node, ImportNode, ImportTag);
    node->module = NULL;
    node->fold = NULL;
    node->ispub = 0;
    node->isextends = 0;
    return node;
}

// Serialize a list of names a clause holds, 'but' ones or listed ones
static void importPrintNames(Nodes *names, int listed) {
    INode **nodesp;
    uint32_t cnt;
    int first = 1;
    for (nodesFor(names, cnt, nodesp)) {
        if (!first)
            inodeFprint(", ");
        first = 0;
        if (!listed) {
            inodeFprint("%s", &((NameUseNode*)*nodesp)->namesym->namestr);
            continue;
        }
        AliasDclNode *alias = (AliasDclNode*)*nodesp;
        Name *srcname = ((NameUseNode*)alias->target)->namesym;
        inodeFprint("%s", &srcname->namestr);
        if (alias->namesym != srcname)
            inodeFprint(" as %s", &alias->namesym->namestr);
    }
}

// Serialize a import node
void importPrint(ImportNode *node) {
    inodeFprint(node->ispub ? "pub import %s" : "import %s",
        node->module? &node->module->namesym->namestr : "stdio");
    FoldClause *fold = node->fold;
    if (fold == NULL)
        return;
    // 'pub import' already made the folds public, so the clause has nothing to add
    inodeFprint(fold->ispub && !node->ispub ? " pub use " : " use ");
    if (fold->star) {
        inodeFprint("*");
        if (fold->excludes) {
            inodeFprint(" but ");
            importPrintNames(fold->excludes, 0);
        }
        return;
    }
    importPrintNames(fold->items, 1);
}

// Is every name of one list in the other? Listed items match on both spellings,
// the source's and the local one; 'but' names on the one spelling they have.
static int importNamesWithin(Nodes *names, Nodes *other, int listed) {
    INode **nodesp, **othersp;
    uint32_t cnt, othercnt;
    for (nodesFor(names, cnt, nodesp)) {
        int found = 0;
        for (nodesFor(other, othercnt, othersp)) {
            if (!listed)
                found = ((NameUseNode*)*nodesp)->namesym == ((NameUseNode*)*othersp)->namesym;
            else {
                AliasDclNode *alias = (AliasDclNode*)*nodesp;
                AliasDclNode *oalias = (AliasDclNode*)*othersp;
                found = alias->namesym == oalias->namesym
                    && ((NameUseNode*)alias->target)->namesym == ((NameUseNode*)oalias->target)->namesym;
            }
            if (found)
                break;
        }
        if (!found)
            return 0;
    }
    return 1;
}

// Do two lists of names hold the same names, in whatever order?
static int importNamesSame(Nodes *a, Nodes *b, int listed) {
    uint32_t acnt = a ? a->used : 0;
    uint32_t bcnt = b ? b->used : 0;
    if (acnt != bcnt)
        return 0;
    if (acnt == 0)
        return 1;
    return importNamesWithin(a, b, listed) && importNamesWithin(b, a, listed);
}

// Does this clause fold nothing at all? No clause, or an empty list
static int importFoldsNothing(FoldClause *fold) {
    return fold == NULL || (!fold->star && fold->items->used == 0);
}

// Do two imports of one module say the same thing? Asked at parse, before a star
// clause's items are made, so a star clause is compared on its 'but' names.
int importSame(ImportNode *a, ImportNode *b) {
    if (a->module != b->module || a->ispub != b->ispub)
        return 0;
    FoldClause *afold = a->fold;
    FoldClause *bfold = b->fold;
    if (importFoldsNothing(afold) || importFoldsNothing(bfold))
        return importFoldsNothing(afold) && importFoldsNothing(bfold);
    if (afold->star != bfold->star || afold->ispub != bfold->ispub)
        return 0;
    if (afold->star)
        return importNamesSame(afold->excludes, bfold->excludes, 0);
    return importNamesSame(afold->items, bfold->items, 1);
}

// Bind the imported module's name in the importing module.
//
// The binding is an ALIAS, not the module node itself, so that it can carry a
// visibility of its own: an import is private to the module that wrote it unless
// the statement says 'pub', exactly as a fold is. Naming the module is then
// reached as a path base like any other binding, because every reader of a
// namespace resolves an alias before acting on what it found.
//
// The binding is positioned at the import statement. It is built once the whole
// statement has been parsed and the module loaded, so the lexer has moved on to
// whatever follows, and a duplicate of the name would otherwise be reported there.
//
// Only 'pub import' reaches this binding. A clause's 'pub use' speaks for the
// names the clause folds, and the module's own name is not one of them.
//
// The binding states a DEPENDENCY of this module, not a part of it, so a module
// that extends this one does not take it (FlagImportName, importStarAdmits): it
// imports the module itself if it names it [Jon 23 Sep]. What the import's 'use'
// clause folds in is a part of this module, and does travel.
void importBindModule(ModuleNode *mod, ImportNode *node) {
    ModuleNode *newmod = node->module;
    NameUseNode *target = newNameUseNode(newmod->namesym);
    inodeLexCopy((INode*)target, (INode*)node);
    target->dclnode = (INode*)newmod;
    AliasDclNode *alias = newNameAliasDclNode(newmod->namesym, (INode*)target);
    inodeLexCopy((INode*)alias, (INode*)node);
    alias->flags |= FlagImportName;
    if (node->ispub)
        alias->flags |= FlagPub;
    modAddNamedNode(mod, newmod->namesym, (INode*)alias);
}

// Fold one name this import admits into the importing module's namespace.
//
// The binding is an alias whose target is the SOURCE'S OWN BINDING rather than
// the declaration at the end of the chain, so the origin is kept and a chain of
// folds reads as the chain it is. Where the source's binding is itself reached
// through a global, this one is reached through the same global: the access that
// lowering builds names the global by its declaration, so it is written the same
// from any module.
//
// An item that cannot be made yet -- its name not in the source, or not public
// there -- WAITS for a later fold pass, since a source read round a cycle of
// imports may hold it then, and is reported only by the pass that reports
// (modFoldAll). Once its target is bound the item is made, collision or not,
// and no later pass makes it again.
static void importFoldItem(ModuleNode *mod, ImportNode *import, AliasDclNode *alias) {
    ModuleNode *src = import->module;
    FoldClause *fold = import->fold;
    NameUseNode *target = (NameUseNode*)alias->target;
    Name *srcname = target->namesym;
    INode *found = namespaceFind(&src->namespace, srcname);
    if (found == NULL) {
        if (!modFoldReporting())
            modFoldWait();
        else
            errorMsgNode((INode*)alias, ErrorNoMbr, "%s has no name %s to fold in.",
                &src->namesym->namestr, &srcname->namestr);
        return;
    }
    // A module publishes itself into its own namespace, and the import has bound
    // that name here already. Folding it again would be a duplicate of the name
    // the import is written with. That is so from parse on, so it is reported
    // at once, and the item is made
    if (found == (INode*)src) {
        target->dclnode = found;
        errorMsgNode((INode*)alias, ErrorBadFold,
            "%s is the module being imported, and the import binds that name already.",
            &srcname->namestr);
        return;
    }
    // Only what the source module shows folds. A binding's own visibility is what
    // is read, so a name the source itself folded in privately does not travel.
    // A module extending the source is inside its boundary and takes its
    // private names too. A binding may yet be made public by a second route
    // round a cycle (modFoldBind), so a private one waits too
    if (inodeIsPrivate(found) && !import->isextends) {
        if (!modFoldReporting())
            modFoldWait();
        else
            errorMsgNode((INode*)alias, ErrorNotPublic, "%s is private to %s, so it does not fold.",
                &srcname->namestr, &src->namesym->namestr);
        return;
    }
    // A source binding reached through a global is reached through that same
    // global here, and the member is spelled as its type names it rather than as
    // the source's fold renamed it -- which is what the lowering reads
    INode *through = aliasDclThrough(found);
    if (through) {
        NameUseNode *srctarget = (NameUseNode*)((AliasDclNode*)found)->target;
        target->namesym = srctarget->namesym;
        target->dclnode = srctarget->dclnode;
        alias->through = through;
    }
    else
        target->dclnode = found;
    // A fold is private to the module that made it unless the import says 'pub',
    // before the statement or in its clause. That is the transit rule, and it is
    // nothing but the visibility rule: what a third module sees through this one
    // is what this one re-exported. A listed item was parsed as a member alias,
    // so neither of the bits it starts with is this binding's: it is reached
    // with no receiver, and its visibility is the import's. What a module's
    // 'extends' folds is as visible here as it is in the base: public names stay
    // public, as part of this module's surface, and private ones stay private
    alias->flags &= 0xffff - (FlagPub | FlagMethFld);
    if (import->isextends ? !inodeIsPrivate(found) : fold->ispub)
        alias->flags |= FlagPub;
    // The same declaration reached a second time by a route the module did not
    // write -- a wildcard, an 'extends' -- is the binding the name has already;
    // written twice, it is refused (modFoldBind)
    INode *prior = modFoldBind(mod, alias);
    if (prior == NULL)
        return;
    // A module that extends another ADDS to it, as a type that extends one does:
    // a name of the base redeclared here would make one name of this module mean
    // two things, depending on which module reached it. Reported at the
    // declaration, which is what has to change
    if (import->isextends && prior->tag != AliasDclTag && prior != (INode*)mod) {
        errorMsgNode(prior, ErrorExtendsOverride,
            "%s is already a name of %s, which extends %s: a module that extends another adds to it rather than redeclaring its names.",
            &alias->namesym->namestr, &mod->namesym->namestr, &src->namesym->namestr);
        return;
    }
    // Anything else holding the name was brought in by something of this
    // module's own -- an import, a fold -- and collides as any two bindings do,
    // reported at whichever of the two a single pass would have met second.
    // What 'extends' brings is never written, so it collides only with a
    // different declaration
    INode *at = modFoldCollisionAt(mod, (INode*)import, (INode*)alias, prior);
    if (import->isextends && at == (INode*)alias)
        errorMsgNode(prior, ErrorDupName,
            "%s is already a name of this module, and %s, which it extends, has that name too. A name of a module is unique.",
            &alias->namesym->namestr, &src->namesym->namestr);
    else
        modFoldDupReport(at, alias, prior);
}

// Does a star clause admit this name of its module? Not Self, not an unnamed
// node, not the module's own lifecycle names, not a name 'but' leaves out, and
// not the module's own name, which it publishes into its own namespace and which
// the import binds already. Otherwise, to an import, every PUBLIC name of the
// module, of whatever kind: a module has one instance, so nothing of it is
// reached through a value. To a module EXTENDING the source, private names too
// -- its declarations and what its folds brought in -- but not the name an
// import of the source binds to its module, which is a dependency of the source
// rather than a part of it [Jon 23 Sep].
static int importStarAdmits(ImportNode *import, Name *name, INode *member) {
    if (name == NULL || name == selfTypeName || name == anonName
        || name == finalName || name == cloneName || name == import->module->namesym
        || foldExcluded(import->fold, name))
        return 0;
    if (import->isextends)
        return !(member->tag == AliasDclTag && (member->flags & FlagImportName));
    return !inodeIsPrivate(member);
}

// Is there nothing for a star clause to do with this name of its module, as it
// stands in this pass? So where the importing module binds it already to what
// the source binds it to, and no more visibly or as a fold than a new route would
// make it; and where this clause met it colliding in an earlier pass, and
// reported it then (the clause keeps an item under the name for that).
static int importStarHas(ModuleNode *mod, ImportNode *import, Name *name, INode *found) {
    INode *prior = namespaceFind(&mod->namespace, name);
    if (prior == NULL)
        return 0;
    if (modFoldSameBinding(prior, found)) {
        if (prior->tag != AliasDclTag)
            return 1;
        int pub = import->isextends ? !inodeIsPrivate(found) : import->fold->ispub;
        return !(pub && !(prior->flags & FlagPub)) && !(prior->flags & FlagImportName);
    }
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(import->fold->items, cnt, itemp))
        if (((AliasDclNode*)*itemp)->namesym == name)
            return 1;
    return 0;
}

// Fold in every name a star clause admits, as far as its module holds them in
// this pass. Every pass reads the module afresh, since one read round a cycle of
// imports holds more in a later pass; an item is made only for a name there is
// something to do with (importStarHas), and kept on the clause.
static void importFoldStar(ModuleNode *mod, ImportNode *import) {
    FoldClause *fold = import->fold;
    Namespace *ns = &import->module->namespace;
    namespaceFor(ns) {
        NameNode *nn = &ns->namenodes[__i];
        if (!importStarAdmits(import, nn->name, nn->node) || importStarHas(mod, import, nn->name, nn->node))
            continue;
        // A module's name is reached with no receiver at all, and its binding's
        // visibility is the import's rather than the target's, so it is the bare
        // alias rather than the member one a type's fold makes
        NameUseNode *target = newMemberUseNode(nn->name);
        inodeLexCopy((INode*)target, fold->at);
        AliasDclNode *alias = newNameAliasDclNode(nn->name, (INode*)target);
        inodeLexCopy((INode*)alias, fold->at);
        // Nobody wrote this name: the clause asked for every name, so where it
        // meets another binding of the same declaration, neither was written
        // twice and the two are one (modFoldBind)
        alias->flags |= FlagUnlisted;
        nodesAdd(&fold->items, (INode*)alias);
        importFoldItem(mod, import, alias);
    }
}

// Fold the names this import admits into the importing module's namespace.
//
// The source's NAMESPACE is what is read, not its declaration list, so a name the
// source itself folded in and re-exported travels on -- a fold writes to the
// namespace, and walking the declarations was what left those names behind.
//
// Run in every fold pass (modFoldAll). A star clause reads its module again, and
// a listed item not yet made tries again. The pass that reports reads nothing
// afresh: it reports each listed item still waiting, and each 'but' naming what
// the module does not have.
void importNameRes(NameResState *pstate, ImportNode *node) {
    if (node->fold == NULL || node->module == NULL)
        return;
    ModuleNode *src = node->module;
    ModuleNode *target = pstate->mod;
    FoldClause *fold = node->fold;
    INode **itemp;
    uint32_t cnt;
    if (fold->star) {
        if (!modFoldReporting()) {
            importFoldStar(target, node);
            return;
        }
        if (fold->excludes) {
            for (nodesFor(fold->excludes, cnt, itemp)) {
                Name *name = ((NameUseNode*)*itemp)->namesym;
                if (namespaceFind(&src->namespace, name) == NULL)
                    errorMsgNode(*itemp, ErrorNoMbr, "%s has no member named %s to leave out.",
                        &src->namesym->namestr, &name->namestr);
            }
        }
        return;
    }
    for (nodesFor(fold->items, cnt, itemp)) {
        if (((NameUseNode*)((AliasDclNode*)*itemp)->target)->dclnode == NULL)
            importFoldItem(target, node, (AliasDclNode*)*itemp);
    }
}

// Type check the import node
void importTypeCheck(TypeCheckState *pstate, ImportNode *node) {
    // Type check the module we are importing
    inodeTypeCheckAny(pstate, (INode**)&node->module);
}
