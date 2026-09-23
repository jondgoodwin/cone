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
    return node;
}

// Serialize a import node
void importPrint(ImportNode *node) {
    inodeFprint("import %s", node->module? &node->module->namesym->namestr : "stdio");
    if (node->fold && node->fold->star)
        inodeFprint(".*");
}

// Bind the imported module's name in the importing module.
//
// The binding is an ALIAS, not the module node itself, so that it can carry a
// visibility of its own: an import is private to the module that wrote it unless
// the statement says 'pub', exactly as a fold is. Naming the module is then
// reached as a path base like any other binding, because every reader of a
// namespace resolves an alias before acting on what it found.
void importBindModule(ModuleNode *mod, ImportNode *node, uint16_t pubflag) {
    ModuleNode *newmod = node->module;
    NameUseNode *target = newNameUseNode(newmod->namesym);
    target->dclnode = (INode*)newmod;
    AliasDclNode *alias = newNameAliasDclNode(newmod->namesym, (INode*)target);
    alias->flags |= pubflag;
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
        errorMsgNode((INode*)alias, ErrorNoMbr, "%s has no name %s to fold in.",
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
    // A fold is private to the module that made it unless the import says 'pub'.
    // That is the transit rule, and it is nothing but the visibility rule: what a
    // third module sees through this one is what this one re-exported
    if (fold->ispub)
        alias->flags |= FlagPub;
    INode *prior = namespaceAdd(&mod->namespace, alias->namesym, (INode*)alias);
    if (prior) {
        errorMsgNode((INode*)alias, ErrorDupName,
            "%s is already a name of this module. A folded name must be unique.",
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
