/** Module and import node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
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

// ---- A re-export lost round a cycle of imports -----------------------------
//
// modFoldNames is dependency-first, and a cycle of imports stops at the module
// whose folds are still running: where A and B import each other, one of them
// folds from the other before the other's own folds are in place, and a name the
// other re-exported is not there yet. That is not changed here -- a re-export
// still does not travel round a cycle -- but it is DIAGNOSED: where a name is
// missing only because of it, the error says so, names the cycle, and points at
// the imports that close it. Where a missing name has nothing to do with a cycle,
// the message is what it always was.
//
// The test is not a guess. An import that read its module mid-fold records the
// cycle (ImportNode.cycle), and once that module's folds have run, the question
// is simply whether it now holds the name: its declarations were all bound at
// parse, so anything it holds that it did not hold then, a fold of its own put
// there. A name found missing while folds are still running is judged when the
// outermost modFoldNames returns, when every module round the cycle is complete.

// The modules whose folds are running, outermost first, and the import each is
// following: foldimps[i] is the import foldmods[i] has recursed through
static Nodes *foldmods = NULL;
static Nodes *foldimps = NULL;

// A missing name held back until the folds round a cycle have run
typedef struct ModMissing {
    struct ModMissing *next;
    INode *at;              // Where the diagnostic goes
    ModuleNode *reader;     // The module that looked the name up
    ModuleNode *mod;        // The module whose namespace did not hold it
    Name *name;             // The name as it was looked up there
    int code;               // Today's diagnostic, if the cycle is not the cause
    char *msg;
} ModMissing;
static ModMissing *missingfirst = NULL;
static ModMissing *missinglast = NULL;

// Which name of a clause's source would this import have folded in under this
// spelling? NULL for none
static Name *modFoldAdmits(ImportNode *import, Name *name) {
    FoldClause *fold = import->fold;
    if (fold->star)
        return foldExcluded(fold, name) || name == import->module->namesym ? NULL : name;
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(fold->items, cnt, itemp)) {
        AliasDclNode *alias = (AliasDclNode*)*itemp;
        if (alias->namesym == name)
            return ((NameUseNode*)alias->target)->namesym;
    }
    return NULL;
}

// Could a name missing from 'mod', looked up from 'reader', be a re-export lost
// round a cycle? Only if the reader read 'mod' itself mid-fold, or 'mod' read
// the source of one of its own clauses mid-fold
static int modMayLoseToCycle(ModuleNode *reader, ModuleNode *mod) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(reader->imports, cnt, nodesp)) {
        ImportNode *import = (ImportNode*)*nodesp;
        if (import->cycle && import->module == mod)
            return 1;
    }
    for (nodesFor(mod->imports, cnt, nodesp)) {
        ImportNode *import = (ImportNode*)*nodesp;
        if (import->cycle && import->fold)
            return 1;
    }
    return 0;
}

// The cycle an import closes, as the modules round it: 'alpha -> beta -> alpha'
static char *modCycleText(ImportNode *import) {
    Nodes *cycle = import->cycle;
    ModuleNode *start = ((ImportNode*)nodesLast(cycle))->module;
    size_t size = start->namesym->namesz + 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(cycle, cnt, nodesp))
        size += ((ImportNode*)*nodesp)->module->namesym->namesz + 4;
    char *text = memAllocBlk(size);
    strcpy(text, &start->namesym->namestr);
    for (nodesFor(cycle, cnt, nodesp)) {
        strcat(text, " -> ");
        strcat(text, &((ImportNode*)*nodesp)->module->namesym->namestr);
    }
    return text;
}

// Report a missing name: as a re-export lost round a cycle of imports where that
// is why it is missing, and otherwise with the message it always had
static void modReportMissing(ModMissing *missing) {
    ModuleNode *reader = missing->reader;
    ModuleNode *mod = missing->mod;
    Name *name = missing->name;
    ImportNode *via = NULL;     // The import that read the re-exporting module mid-fold
    ModuleNode *folder = NULL;  // The module that wrote that import
    Name *srcname = NULL;       // The name as the re-exporting module spells it
    INode *binding = NULL;      // Its binding there, now that its folds have run
    INode **nodesp;
    uint32_t cnt;

    // The reader read 'mod' itself mid-fold, and 'mod' holds the name now
    for (nodesFor(reader->imports, cnt, nodesp)) {
        ImportNode *import = (ImportNode*)*nodesp;
        if (import->cycle == NULL || import->module != mod)
            continue;
        INode *found = namespaceFind(&mod->namespace, name);
        if (found && !inodeIsPrivate(found)) {
            via = import;
            folder = reader;
            srcname = name;
            binding = found;
        }
        break;
    }
    // Or 'mod' is missing it because one of its own clauses read its source
    // mid-fold, and would have admitted the name that source holds now. From
    // outside 'mod', only a 'pub' clause would have made it reachable
    if (via == NULL) {
        for (nodesFor(mod->imports, cnt, nodesp)) {
            ImportNode *import = (ImportNode*)*nodesp;
            if (import->cycle == NULL || import->fold == NULL || import->module == NULL)
                continue;
            if (reader != mod && !import->fold->ispub)
                continue;
            Name *admitted = modFoldAdmits(import, name);
            if (admitted == NULL)
                continue;
            INode *found = namespaceFind(&import->module->namespace, admitted);
            if (found == NULL || inodeIsPrivate(found) || found == (INode*)import->module)
                continue;
            via = import;
            folder = mod;
            srcname = admitted;
            binding = found;
            break;
        }
    }
    if (via == NULL) {
        errorMsgNode(missing->at, missing->code, "%s", missing->msg);
        return;
    }

    ModuleNode *src = via->module;
    char *cycletext = modCycleText(via);
    if (name == srcname)
        errorMsgNode(missing->at, ErrorCircular,
            "%s is re-exported by %s, but %s read %s round a cycle of imports (%s), before %s's own folds had run. A re-export does not travel round a cycle of imports.",
            &name->namestr, &src->namesym->namestr, &folder->namesym->namestr,
            &src->namesym->namestr, cycletext, &src->namesym->namestr);
    else
        errorMsgNode(missing->at, ErrorCircular,
            "%s names %s, which %s re-exports, but %s read %s round a cycle of imports (%s), before %s's own folds had run. A re-export does not travel round a cycle of imports.",
            &name->namestr, &srcname->namestr, &src->namesym->namestr, &folder->namesym->namestr,
            &src->namesym->namestr, cycletext, &src->namesym->namestr);

    // The imports that close the cycle, and the fold that had not run
    ModuleNode *importer = src;
    for (nodesFor(via->cycle, cnt, nodesp)) {
        ImportNode *import = (ImportNode*)*nodesp;
        errorMsgNode((INode*)import, Uncounted, "... %s imports %s here",
            &importer->namesym->namestr, &import->module->namesym->namestr);
        importer = import->module;
    }
    errorMsgNode(binding, Uncounted, "... and %s re-exports %s here, in a fold that had not yet run",
        &src->namesym->namestr, &srcname->namestr);
}

// Report every missing name held back while folds were running
static void modReportHeldMissing() {
    ModMissing *missing = missingfirst;
    missingfirst = missinglast = NULL;
    for (; missing; missing = missing->next)
        modReportMissing(missing);
}

// Report a name missing from a module's namespace, looked up from 'reader'.
// Where the cause may be a re-export lost round a cycle of imports, the report
// says so (modReportMissing), held back until the folds round the cycle have run
// if they are running still; otherwise it is the message given, reported now.
void modNameMissing(ModuleNode *reader, ModuleNode *mod, Name *name, INode *at, int code, const char *msg, ...) {
    char text[1024];
    va_list args;
    va_start(args, msg);
    vsnprintf(text, sizeof(text), msg, args);
    va_end(args);

    if (reader == NULL || mod == NULL || !modMayLoseToCycle(reader, mod)) {
        errorMsgNode(at, code, "%s", text);
        return;
    }
    ModMissing *missing = memAllocBlk(sizeof(ModMissing));
    missing->next = NULL;
    missing->at = at;
    missing->reader = reader;
    missing->mod = mod;
    missing->name = name;
    missing->code = code;
    missing->msg = memAllocStr(text, strlen(text));
    if (foldmods && foldmods->used > 0) {
        if (missinglast)
            missinglast->next = missing;
        else
            missingfirst = missing;
        missinglast = missing;
        return;
    }
    modReportMissing(missing);
}

// The imports round the cycle an import closes: its module's folds are running,
// so that module is on the fold path, and the cycle runs from the import it is
// following to this one
static Nodes *modFoldCycle(ImportNode *import) {
    uint32_t start = 0;
    while (start < foldmods->used && nodesGet(foldmods, start) != (INode*)import->module)
        ++start;
    Nodes *cycle = newNodes(4);
    for (uint32_t i = start; i < foldimps->used; ++i)
        nodesAdd(&cycle, nodesGet(foldimps, i));
    nodesAdd(&cycle, (INode*)import);
    return cycle;
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
// is missing on the way round is a re-export, never a declaration. The import
// that met the mark records the cycle, so that a name missing for that reason is
// reported as one (modNameMissing).
void modFoldNames(NameResState *pstate, ModuleNode *mod) {
    if (mod->foldstate != 0)
        return;
    mod->foldstate = 1;
    if (foldmods == NULL) {
        foldmods = newNodes(8);
        foldimps = newNodes(8);
    }
    nodesAdd(&foldmods, (INode*)mod);

    ModuleNode *owningmod = pstate->mod;
    pstate->mod = mod;
    modHook(NULL, mod);

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->imports, cnt, nodesp)) {
        ImportNode *import = (ImportNode*)*nodesp;
        if (import->module && import->module->foldstate == 1)
            import->cycle = modFoldCycle(import);
        else if (import->module) {
            nodesAdd(&foldimps, (INode*)import);
            modFoldNames(pstate, import->module);
            --foldimps->used;
        }
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

    // Every module this one reached is complete now, round any cycle included
    --foldmods->used;
    if (foldmods->used == 0)
        modReportHeldMissing();
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
    // An enum that extends another holds copies of its base's variants that are
    // no module's nodes, and they are reached through it.
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        inodeTypeCheckAny(pstate, nodesp);
        if ((*nodesp)->tag == StructTag)
            structEnumCheckCopies(pstate, (StructNode*)*nodesp);
    }
}
