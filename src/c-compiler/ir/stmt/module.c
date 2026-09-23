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
    mod->filesym = NULL;
    mod->foldersym = NULL;
    mod->imports = newNodes(8);
    mod->enumuses = newNodes(4);
    mod->nodes = newNodes(64);
    namespaceInit(&mod->namespace, 64);
    dclInfoInit(&mod->dclinfo);
    mod->foldstate = 0;
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
    // A 'use' of an enum is neither a declaration nor a field: it declares
    // bindings, and those are made in the fold pass, so it is held beside the
    // imports and never joins the walks
    if (node->tag == EnumUseTag) {
        nodesAdd(&mod->enumuses, node);
        return;
    }

    // Add to regular ordered node list, and record the module as the
    // declaration's owner. An imported module never comes this way: it is
    // declared program-wide and only bound here (modAddNamedNode), so it keeps
    // no owner.
    nodesAdd(&mod->nodes, node);
    dclInfoJoin(node, (INode*)mod);
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

    // The root module is the one that contributes no name to the owner chain
    if (mod->dclinfo.facts & DclNamesChain)
        inodeFprint("module %s", &mod->namesym->namestr);
    else
        inodeFprint("IR for program %s", mod->lexer->url);
    dclInfoPrint((INode*)mod);
    inodeFprint("\n");
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

// Put every name this module holds by FOLDING into its namespace, and do it
// before ANY module's own nodes are name resolved.
//
// What a module holds by folding used to arrive at the start of that module's
// own resolution, and modules were resolved in the order they were loaded -- so
// a module loaded before the one that folded a name could not reach it through
// the qualifier and a module loaded after could, and the root, which loads
// first, could reach none of them. That was load order deciding what a name
// means, which is not something a program may depend on.
//
// DEPENDENCY-FIRST, and that is what makes transit work rather than a rule that
// makes it work: a module's own folds are complete before anything folds FROM
// it, so what this module re-exported is what the next one finds. A cycle stops
// at the mark -- the module's own declarations are all bound at parse, so what
// is missing on the way round is a re-export, never a declaration.
void modFoldNames(NameResState *pstate, ModuleNode *mod) {
    if (mod->foldstate != 0)
        return;
    mod->foldstate = 1;

    ModuleNode *owningmod = pstate->mod;
    pstate->mod = mod;
    modHook(NULL, mod);

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->imports, cnt, nodesp)) {
        ImportNode *import = (ImportNode*)*nodesp;
        if (import->module)
            modFoldNames(pstate, import->module);
        // The recursion above swapped the hook and this module's is current again
        importNameRes(pstate, import);
    }

    // A global's 'use' clause folds names of its type into this namespace, and
    // that happens before the module's other nodes are resolved so that a folded
    // name is in place wherever it is used -- including in a function declared
    // above the global that folded it, since a module's names do not depend on
    // the order they were written in. Each such global is resolved here and left
    // out of the walk in modNameRes.
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if ((*nodesp)->tag != VarDclTag || ((VarDclNode*)*nodesp)->fold == NULL)
            continue;
        inodeNameRes(pstate, nodesp);
        foldGlobalExpand(pstate, mod, (VarDclNode*)*nodesp);
    }

    // A 'use' of an enum folds its variants in as names of this module. Last,
    // so the enum may be named through anything the imports and the globals
    // folded in, and before any body resolves, so a variant's bare name is in
    // place wherever it is used
    for (nodesFor(mod->enumuses, cnt, nodesp))
        foldEnumUseExpand(pstate, mod, (EnumUseNode*)*nodesp);

    modHook(mod, NULL);
    pstate->mod = owningmod;
    mod->foldstate = 2;
}

// Name resolution of the module node. Modules are resolved in the order they
// were loaded, the root first -- and what that order no longer decides is what
// a name reaches, because every module's folds are in place before the first of
// them starts (modFoldNames). A type in a module not yet reached may be resolved
// earlier, by demand from a type that is-a it (structNameResDemand).
void modNameRes(NameResState *pstate, ModuleNode *mod) {
    ModuleNode *owningmod = pstate->mod;
    pstate->mod = mod;

    // Switch name table over to new module
    modHook(NULL, mod);

    INode **nodesp;
    uint32_t cnt;
    mod->flags |= NameResolving;

    // A type alias names a type expression, and a use of the alias asks what is
    // at the end of that chain. Resolved ahead of the walk for the same reason a
    // fold is: a forward reference to a typedef is ordinary, so the target has to
    // be bound before anything asks whether the name is a type at all.
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if ((*nodesp)->tag == AliasDclTag && ((*nodesp)->flags & FlagTypeAlias))
            inodeNameRes(pstate, nodesp);
    }
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if ((*nodesp)->tag == AliasDclTag && ((*nodesp)->flags & FlagTypeAlias))
            aliasDclCheckCycle((AliasDclNode*)*nodesp);
    }

    for (nodesFor(mod->nodes, cnt, nodesp)) {
        // Resolved by one of the passes above
        if ((*nodesp)->tag == VarDclTag && ((VarDclNode*)*nodesp)->fold != NULL)
            continue;
        if ((*nodesp)->tag == AliasDclTag && ((*nodesp)->flags & FlagTypeAlias))
            continue;
        inodeNameRes(pstate, nodesp);
    }
    mod->flags = (mod->flags & ~NameResolving) | NameResolved;

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
