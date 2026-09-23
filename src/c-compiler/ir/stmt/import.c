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
    node->cycle = NULL;
    node->ispub = 0;
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
    inodeFprint(fold->ispub && !node->ispub ? " use pub " : " use ");
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
// Only 'pub import' reaches this binding. A clause's 'use pub' speaks for the
// names the clause folds, and the module's own name is not one of them.
void importBindModule(ModuleNode *mod, ImportNode *node) {
    ModuleNode *newmod = node->module;
    NameUseNode *target = newNameUseNode(newmod->namesym);
    inodeLexCopy((INode*)target, (INode*)node);
    target->dclnode = (INode*)newmod;
    AliasDclNode *alias = newNameAliasDclNode(newmod->namesym, (INode*)target);
    inodeLexCopy((INode*)alias, (INode*)node);
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
static void importFoldItem(ModuleNode *mod, ModuleNode *src, FoldClause *fold, AliasDclNode *alias) {
    NameUseNode *target = (NameUseNode*)alias->target;
    Name *srcname = target->namesym;
    INode *found = namespaceFind(&src->namespace, srcname);
    if (found == NULL) {
        modNameMissing(mod, src, srcname, (INode*)alias, ErrorNoMbr, "%s has no name %s to fold in.",
            &src->namesym->namestr, &srcname->namestr);
        return;
    }
    // A module publishes itself into its own namespace, and the import has bound
    // that name here already. Folding it again would be a duplicate of the name
    // the import is written with
    if (found == (INode*)src) {
        errorMsgNode((INode*)alias, ErrorBadFold,
            "%s is the module being imported, and the import binds that name already.",
            &srcname->namestr);
        return;
    }
    // Only what the source module shows folds. A binding's own visibility is what
    // is read, so a name the source itself folded in privately does not travel
    if (inodeIsPrivate(found)) {
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
    // with no receiver, and its visibility is the import's
    alias->flags &= 0xffff - (FlagPub | FlagMethFld);
    if (fold->ispub)
        alias->flags |= FlagPub;
    INode *prior = namespaceAdd(&mod->namespace, alias->namesym, (INode*)alias);
    if (prior) {
        errorMsgNode((INode*)alias, ErrorDupName,
            "%s is already a name of this module. A folded name must be unique: rename it with 'as', or leave it out with 'but'.",
            &alias->namesym->namestr);
        return;
    }
    nametblHookNode(alias->namesym, (INode*)alias);
}

// Fold the names this import admits into the importing module's namespace.
//
// The source's NAMESPACE is what is read, not its declaration list, so a name the
// source itself folded in and re-exported travels on -- a fold writes to the
// namespace, and walking the declarations was what left those names behind.
void importNameRes(NameResState *pstate, ImportNode *node) {
    if (node->fold == NULL || node->module == NULL)
        return;
    ModuleNode *src = node->module;
    ModuleNode *target = pstate->mod;
    if (node->fold->expanded)
        return;
    node->fold->expanded = 1;
    if (node->fold->star)
        foldStarItems(&src->namespace, src->namesym, node->fold, FoldAdmitNames);
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(node->fold->items, cnt, itemp))
        importFoldItem(target, src, node->fold, (AliasDclNode*)*itemp);
}

// Type check the import node
void importTypeCheck(TypeCheckState *pstate, ImportNode *node) {
    // Type check the module we are importing
    inodeTypeCheckAny(pstate, (INode**)&node->module);
}
