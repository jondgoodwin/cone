/** Module and import node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new Module node
ModuleNode *newModuleNode() {
    ModuleNode *mod;
    newNode(mod, ModuleNode, ModuleTag);
    mod->namesym = NULL;
    mod->owner = NULL;
    mod->nodes = newNodes(64);
    namespaceInit(&mod->namednodes, 64);
    return mod;
}

// Add a newly parsed named node to the module:
// - We preserve all nodes for later semantic pass and serialization iteration
//     Name resolution will iterate over these even to pick up folder names/aliases
// - We hook all names in global name table at parse time to check for name dupes and
//     because permissions and allocators do not support forward references
// - We remember all public names for later resolution of qualified names
void modAddNode(ModuleNode *mod, INode *node) {

    // Add to regular ordered node list
    nodesAdd(&mod->nodes, node);

    // If it is a named node...
    if (isNamedNode(node)) {
        INamedNode *nnode = (INamedNode *)node;
        Name *name = nnode->namesym;

        // Hook into global name table (and add to namednodes), if not already there
        if (!name->node) {
            nametblHookNode(nnode);
            // Remember public names
            if (name->namestr != '_')
                namespaceSet(&mod->namednodes, name, nnode);
        }
        else {
            errorMsgNode((INode *)node, ErrorDupName, "Global name is already defined. Duplicates not allowed.");
            errorMsgNode((INode*)name->node, ErrorDupName, "This is the conflicting definition for that name.");
        }
    }
}

// Serialize a module node
void modPrint(ModuleNode *mod) {
    INode **nodesp;
    uint32_t cnt;

    if (mod->namesym)
        inodeFprint("module %s\n", &mod->namesym->namestr);
    else
        inodeFprint("IR for program %s\n", mod->lexer->url);
    inodePrintIncr();
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        inodePrintIndent();
        inodePrintNode(*nodesp);
        inodePrintNL();
    }
    inodePrintDecr();
}

// Unhook old module's names, hook new module's names
// (works equally well from parent to child or child to parent
void modHook(ModuleNode *oldmod, ModuleNode *newmod) {
    if (oldmod)
        nametblHookPop();
    if (newmod) {
        INode **nodesp;
        uint32_t cnt;
        nametblHookPush();
        for (nodesFor(newmod->nodes, cnt, nodesp)) {
            if (isNamedNode(*nodesp))
                nametblHookNode((INamedNode *)*nodesp);
        }
    }
}

// Name resolution of the module node
void modNameRes(NameResState *pstate, ModuleNode *mod) {

    // Switch name table over to new module
    modHook((ModuleNode*)mod->owner, mod);

    // Process all nodes
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        inodeNameRes(pstate, nodesp);
    }

    // Switch name table back to owner module
    modHook(mod, (ModuleNode*)mod->owner);
}

// Type check the module node
void modTypeCheck(TypeCheckState *pstate, ModuleNode *mod) {
    // Process all nodes
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        inodeTypeCheck(pstate, nodesp);
    }
}