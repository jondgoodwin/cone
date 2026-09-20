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
    node->foldall = 0;
    return node;
}

// Serialize a import node
void importPrint(ImportNode *node) {
    inodeFprint("import %s", node->module? &node->module->namesym->namestr : "stdio");
    if (node->foldall)
        inodeFprint("::*");
}

// The name under which a wildcard import folds a node of the source module, or
// NULL for a node it does not fold.
//
// A private name is not folded: refmodule.html says such a name may not be
// referenced outside its module, and generation agrees by emitting no symbol
// for one whose module this compile does not generate. A private *candidate*
// of a public overload name still arrives, because the overload name is what
// folds and the candidate travels inside it.
//
// A module's nodes are not all things a source named. An anonymous function is
// lifted here while parsing so that it is generated, and it carries no name at
// all -- there is nothing for an importer to fold, and handing that NULL to the
// namespace crashed the compile of any file wildcard-importing a module that
// contained one. It is reached through the reference the source wrote, which
// travels with the expression.
static Name *importFoldName(INode *node) {
    if (!isNamedNode(node) || inodeIsPrivate(node))
        return NULL;
    return inodeGetName(node);
}

// Name resolution of the import node: fold the source module's public names
// into the importing module's namespace
void importNameRes(NameResState *pstate, ImportNode *node) {
    if (!node->foldall || !node->module)
        return;

    ModuleNode *sourcemod = node->module;
    ModuleNode *targetmod = pstate->mod;

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sourcemod->nodes, cnt, nodesp)) {
        Name *name = importFoldName(*nodesp);
        if (name)
            modAddNamedNode(targetmod, name, *nodesp);
    }
}

// Hook, in the current hook table, every name this import will fold when its
// module is resolved, without folding it. For a type resolved by demand ahead
// of its module (structNameResDemand): the type's bodies must see what the
// module's wildcard imports bring in, and the module's namespace must not gain
// those names before its own resolution folds them, since what a qualifier can
// reach in a module is decided by that fold and when it ran.
void importHookFolds(ImportNode *node) {
    if (!node->foldall || !node->module)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->module->nodes, cnt, nodesp)) {
        Name *name = importFoldName(*nodesp);
        if (name)
            nametblHookNode(name, *nodesp);
    }
}

// Type check the import node
void importTypeCheck(TypeCheckState *pstate, ImportNode *node) {
    // Type check the module we are importing
    inodeTypeCheckAny(pstate, (INode**)&node->module);
}
