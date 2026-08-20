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
    mod->imports = newNodes(8);
    mod->nodes = newNodes(64);
    namespaceInit(&mod->namespace, 64);
    return mod;
}

// Add a newly parsed named node to the module:
// - We hook all names in global name table at parse time to check for name dupes and
//     because permissions and allocators do not support forward references
// - We remember all public names for later resolution of qualified names
void modAddNamedNode(ModuleNode *mod, Name *name, INode *node) {

    // Hook into global name table (and add to namednodes), if not already there
    if (!name->node) {
        nametblHookNode(name, (INode*)node);
        namespaceSet(&mod->namespace, name, node);
    }
    else {
        errorMsgNode((INode *)node, ErrorDupName, "Global name is already defined. Duplicates not allowed.");
        errorMsgNode((INode*)name->node, ErrorDupName, "This is the conflicting definition for that name.");
    }
}

// Add a newly parsed named node to the module:
// - We preserve all nodes for later semantic pass and serialization iteration
//     Name resolution will iterate over these even to pick up folder names/aliases
// - We hook all names in global name table at parse time to check for name dupes and
//     because permissions and allocators do not support forward references
// - We remember all public names for later resolution of qualified names
void modAddNode(ModuleNode *mod, Name *name, INode *node) {

    // imports need to be processed, as name folding must be finished
    // before we do name resolution on module's nodes
    if (node->tag == ImportTag) {
        nodesAdd(&mod->imports, node);
        return;
    }

    // Add to regular ordered node list
    nodesAdd(&mod->nodes, node);
    // '_' binds nothing, matching namespaceAdd. A declaration the parser could
    // not name carries it, and hooking that would both make '_' resolve to the
    // declaration and make a second unnamed one a duplicate of the first.
    if (name && name != anonName)
        modAddNamedNode(mod, name, node);
}

// Add a parsed function to the module:
// - The concrete FnDclNode is always owned by the module and bound to its unique name
// - An overload name binds separately to its own FnOverloadDclNode, which is also owned
//     by the module so that it is printed and can be folded in by a wildcard import
void modAddFn(ModuleNode *mod, FnDclNode *fnnode) {
    modAddNode(mod, fnnode->namesym, (INode*)fnnode);

    if (fnnode->overloadsym == NULL)
        return;

    INode *binding = namespaceFind(&mod->namespace, fnnode->overloadsym);
    if (binding == NULL) {
        FnOverloadDclNode *overloadnode = newFnOverloadDclNode(fnnode->overloadsym);
        inodeLexCopy((INode*)overloadnode, (INode*)fnnode);
        fnOverloadDclAdd(overloadnode, fnnode);
        modAddNode(mod, overloadnode->namesym, (INode*)overloadnode);
        return;
    }
    if (binding->tag != FnOverloadDclTag) {
        errorMsgNode((INode*)fnnode, ErrorOverloadClash,
            "Overload name %s is already declared as something that is not an overload name.",
            &fnnode->overloadsym->namestr);
        return;
    }
    fnOverloadDclAdd((FnOverloadDclNode*)binding, fnnode);
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
        nametblHookPush();
        nametblHookNamespace(&newmod->namespace);
    }
}

// Name resolution of the module node
void modNameRes(NameResState *pstate, ModuleNode *mod) {
    ModuleNode *owningmod = pstate->mod;
    pstate->mod = mod;

    // Switch name table over to new module
    modHook(NULL, mod);

    // Process all nodes
    INode **nodesp;
    uint32_t cnt;
    // Do name folding of imports, before we name resolve rest of module
    for (nodesFor(mod->imports, cnt, nodesp)) {
        inodeNameRes(pstate, nodesp);
    }
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        inodeNameRes(pstate, nodesp);
    }

    // Switch name table back to owner module
    modHook(mod, NULL);
    pstate->mod = owningmod;
}

// Type check the module node
void modTypeCheck(TypeCheckState *pstate, ModuleNode *mod) {
    INode **nodesp;
    uint32_t cnt;

    // Type check any imported modules this module depends on first
    for (nodesFor(mod->imports, cnt, nodesp)) {
        inodeTypeCheckAny(pstate, nodesp);
    }

    // Then analyze every declaration this module holds, in the order written.
    //
    // There used to be two passes here: every declaration's signature first, so
    // that a forward reference had a type to read, and only then the bodies.
    // Reaching a name now analyzes the declaration it names, so a forward
    // reference pulls what it needs forward itself -- including the case the
    // pre-pass could never serve, a global whose type comes from its own initial
    // value and so is not known until that value is analyzed.
    //
    // Order still does not decide what is analyzed, only when: this loop reaches
    // every declaration, and one already analyzed by demand returns at once.
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        inodeTypeCheckAny(pstate, nodesp);
    }
}
