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

// Name resolution of the import node
void importNameRes(AnalysisState *pstate, ImportNode *node) {
    if (!node->foldall || !node->module)
        return;

    ModuleNode *sourcemod = node->module;
    ModuleNode *targetmod = pstate->mod;

    // Process all nodes.
    //
    // A private name is not folded: refmodule.html says such a name may not be
    // referenced outside its module, and generation agrees by emitting no symbol
    // for one whose module this compile does not generate. A private *candidate*
    // of a public overload name still arrives, because the overload name is what
    // folds and the candidate travels inside it.
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sourcemod->nodes, cnt, nodesp)) {
        if (!isNamedNode(*nodesp) || inodeIsPrivate(*nodesp))
            continue;
        modAddNamedNode(targetmod, inodeGetName(*nodesp), *nodesp);
    }
}

// Type check the import node
void importTypeCheck(AnalysisState *pstate, ImportNode *node) {
    // Type check the module we are importing
    inodeTypeCheckAny(pstate, (INode**)&node->module);
}
