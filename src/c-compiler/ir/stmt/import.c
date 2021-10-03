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
void importNameRes(NameResState *pstate, ImportNode *node) {
    if (!node->foldall || !node->module)
        return;

    ModuleNode *sourcemod = node->module;
    ModuleNode *targetmod = pstate->mod;

    // Process all nodes
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sourcemod->nodes, cnt, nodesp)) {
        if (!isNamedNode(*nodesp))
            continue;
        modAddNamedNode(targetmod, inodeGetName(*nodesp), *nodesp);
    }
}

// Type check the import node
void importTypeCheck(TypeCheckState *pstate, ImportNode *node) {
    // Type check the module we are importing
    inodeTypeCheckAny(pstate, (INode**)&node->module);
}
