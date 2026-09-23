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
    mod->extendsname = NULL;
    mod->extends = NULL;
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

// What a binding of a module's namespace stands for: the declaration at the end
// of a chain of fold aliases, or the binding itself where it is a declaration.
// A typedef is a declaration of its own, and the chain stops there: its target
// is resolved with the module's other nodes, after every fold has run
static INode *modBindingDcl(INode *node) {
    while (node && node->tag == AliasDclTag && !(node->flags & FlagTypeAlias)) {
        INode *target = ((AliasDclNode*)node)->target;
        node = (target && isNameUseNode(target)) ? ((NameUseNode*)target)->dclnode : NULL;
    }
    return node;
}

// Is this binding one of the same declaration, by the same route, as 'alias'?
static int modFoldSameDcl(INode *prior, AliasDclNode *alias) {
    INode *dcl = modBindingDcl((INode*)alias);
    return dcl != NULL && dcl == modBindingDcl(prior) && aliasDclThrough(prior) == alias->through;
}

// Did the module's own source write this binding under its name? Everything but
// what a star clause made: a declaration, a typedef, the name an import binds to
// its module, a listed item of a clause, and an enum's 'use'
static int modBindingWritten(INode *node) {
    return !(node->tag == AliasDclTag && (node->flags & FlagUnlisted));
}

// Bind a name a fold brings into a module's namespace -- an import's clause, a
// module's 'extends', a global's clause, an enum's 'use' -- and hook it. NULL
// once it is bound; otherwise the binding already holding the name, which the
// caller reports as a collision (modFoldDupReport).
//
// A name the module's own source WRITES twice is an error, whatever the two
// stand for: the same name listed twice in a clause, two identical 'use
// Colors;', a name both imported and listed, a listed name that is the module's
// own declaration. Two ways of bringing in the same thing is a cleanliness
// issue [Jon 23 Sep]. (Two imports of one module are refused at parse, where the
// module's name is bound: parseImport.)
//
// A name the module never wrote -- one a wildcard 'use *', an 'extends' or the
// implicit core import brought -- is one binding with another of the SAME
// declaration under that name: the second is the binding the name has already
// [Jon 23 Sep]. That is what lets a name reach a module by two routes nobody
// spelled -- the diamond, two wildcard imports that each re-export it, a
// module's own declaration handed back by a module that extends it, or the core
// fold an extending module takes from its base meeting its own -- and it keeps
// the order the folds ran in from deciding whether a program compiles. One side
// unwritten is enough: a listed name meeting a wildcard's arrival of the same
// declaration was not written twice. 'The same' is the same declaration reached
// the same way: a member folded through two different globals is two things, and
// collides, as two different declarations do everywhere.
//
// Where one route is public and the other private, the binding is public, so a
// re-export is not lost to whichever route happened to be folded first. A
// declaration of the module keeps its own visibility: nothing folds a private
// name of it back as a public one. Where either route was written, the binding
// counts as written, so a third that writes the name again is refused whichever
// order the three were folded in.
//
// The same holds for the kind of binding. An import's binding of its module's
// name is a dependency, which a module extending this one does not take; where a
// wildcard has brought the same module in under that name too, the binding is
// also a fold, and it does travel (FlagImportName).
INode *modFoldBind(ModuleNode *mod, AliasDclNode *alias) {
    INode *prior = namespaceAdd(&mod->namespace, alias->namesym, (INode*)alias);
    if (prior == NULL) {
        nametblHookNode(alias->namesym, (INode*)alias);
        return NULL;
    }
    if (!modFoldSameDcl(prior, alias))
        return prior;
    if (modBindingWritten(prior) && modBindingWritten((INode*)alias))
        return prior;
    if (prior->tag == AliasDclTag) {
        if (alias->flags & FlagPub)
            prior->flags |= FlagPub;
        if (modBindingWritten((INode*)alias))
            prior->flags &= 0xffff - FlagUnlisted;
        prior->flags &= 0xffff - FlagImportName;
    }
    return NULL;
}

// Report the binding modFoldBind found holding a fold's name. Where both stand
// for the same declaration, the module wrote one thing twice; otherwise the name
// means two things
void modFoldDupReport(AliasDclNode *alias, INode *prior) {
    if (modFoldSameDcl(prior, alias))
        errorMsgNode((INode*)alias, ErrorDupName,
            "%s is already a name of this module, for the same thing. A module brings a name in one way: leave out the second.",
            &alias->namesym->namestr);
    else
        errorMsgNode((INode*)alias, ErrorDupName,
            "%s is already a name of this module. A folded name must be unique: rename it with 'as', or leave it out with 'but'.",
            &alias->namesym->namestr);
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
    if (mod->extendsname)
        inodeFprint(" extends %s", &((NameUseNode*)mod->extendsname)->namesym->namestr);
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

// ---- 'mod A extends B': one module reusing another -------------------------
//
// A module that extends another takes in the other's names -- all but the ones
// its imports bind to their modules -- and adds its own declarations beside
// them. For a module, extending and inheriting are one
// thing: static folding ALIASES and keeps the original owner, and a module's
// state is all static -- one instance, at a fixed address -- so there is no
// second copy to make, and each name of B becomes an alias in A's namespace
// whose declaration, symbol and state stay B's.
//
// B's DECLARATIONS and B's FOLDS come across -- what B declares, and every alias
// a 'use' clause of B's made, an import's, a global's or an enum's -- but not the
// name an import of B's binds to its module [Jon 23 Sep]. A module's imports are
// its dependencies, not its contents: scoped imports exist so that each module
// states its own, so A names c1 only if A imports c1, whatever B imports. What
// 'import c1 use cx' folds into B is a name of B, and cx does come across.
//
// Each alias carries B's visibility: A is inside B's boundary, as an enriching
// type is inside its base's, so A's code reads B's private names too [Jon 23
// Sep]. What A shows is what B shows: a name public in B is public in A, because
// what a module extends is part of its own surface, and a name private to B is
// private in A, so A's importers see B's public surface and nothing more. A name
// A declares that B already has, public or private, is refused, as it is for a
// type.
//
// The fold is an ImportNode marked 'isextends' and held on the module rather
// than on its imports, so modFoldNames runs it dependency-first like any import
// and a chain of 'extends' transits: what B took from C is a name of B.

// Resolve what this module's 'extends' names, and refuse what cannot be reused.
//
// What it names is a module this module can ALREADY reach, which is the same
// two places an import's name is answered, looked up rather than loaded: this
// module's own namespace, where an import bound the module's name, and then the
// registry its parent is, which holds its sisters. Nothing is located or read:
// 'extends' states a dependency on a module in reach, and a module out of reach
// is named by an import first.
void modExtendsResolve(ModuleNode *mod) {
    if (mod->extendsname == NULL)
        return;
    NameUseNode *name = (NameUseNode*)mod->extendsname;
    ModuleNode *parent = (ModuleNode*)mod->dclinfo.owner;
    INode *binding = namespaceFind(&mod->namespace, name->namesym);
    if (binding == NULL && parent != NULL)
        binding = namespaceFind(&parent->namespace, name->namesym);
    if (binding == NULL) {
        errorMsgNode((INode*)name, ErrorUnkName,
            "%s names no module this module can reach. A module extends a sister, which its parent holds, or a module it imports.",
            &name->namesym->namestr);
        return;
    }
    INode *found = aliasDclResolve(binding);
    if (found == (INode*)mod) {
        errorMsgNode((INode*)name, ErrorModExtends,
            "A module cannot extend itself.");
        return;
    }
    if (found != NULL && found == (INode*)parent) {
        // Containment runs one way, as it does for an import
        errorMsgNode((INode*)name, ErrorModReach,
            "Module %s is this module's parent, and a module may not extend the module that contains it.",
            &name->namesym->namestr);
        return;
    }
    if (found != NULL && found->tag == StructTag && (found->flags & TraitType)) {
        errorMsgNode((INode*)name, ErrorModExtends,
            "%s is a trait. A module's 'extends' reuses a concrete module; a module conforming to a module trait, as in 'mod arena extends Region', is a different reading and is not built.",
            &name->namesym->namestr);
        return;
    }
    if (found == NULL || found->tag != ModuleTag) {
        errorMsgNode((INode*)name, ErrorModExtends,
            "%s is not a module. A module extends another module; a type is enriched by a type's 'extends'.",
            &name->namesym->namestr);
        return;
    }
    ModuleNode *base = (ModuleNode*)found;
    // A module this one contains is a part of it rather than something it adds
    // to. Folding a submodule's names into its parent is not something any other
    // spelling does either, so it is refused rather than made a second route
    for (ModuleNode *up = (ModuleNode*)base->dclinfo.owner; up; up = (ModuleNode*)up->dclinfo.owner) {
        if (up == mod) {
            errorMsgNode((INode*)name, ErrorModExtends,
                "Module %s is inside this one. A module extends a module beside it, not one it contains.",
                &name->namesym->namestr);
            return;
        }
    }
    name->dclnode = (INode*)base;

    ImportNode *fold = newImportNode();
    inodeLexCopy((INode*)fold, (INode*)name);
    fold->module = base;
    fold->isextends = 1;
    fold->fold = newFoldClause();
    inodeLexCopy(fold->fold->at, (INode*)name);
    // Every declaration and fold of the base, each as visible here as it is
    // there: the clause carries no 'pub' of its own (importFoldItem), and its
    // star leaves out what the base's imports bind (foldStarItems)
    fold->fold->star = 1;
    mod->extends = fold;
}

// Refuse a module whose chain of 'extends' comes back to it: each would be
// adding to the other, and neither has a surface to start from
void modExtendsCheckCycle(ModuleNode *mod, uint32_t nmods) {
    if (mod->extends == NULL)
        return;
    ModuleNode *base = mod->extends->module;
    for (uint32_t i = 0; i < nmods && base != NULL; ++i) {
        if (base == mod) {
            errorMsgNode(mod->extendsname, ErrorModExtends,
                "Module %s extends a module that extends it in turn. A chain of 'extends' may not come back to where it started.",
                &mod->namesym->namestr);
            mod->extends = NULL;
            return;
        }
        base = base->extends ? base->extends->module : NULL;
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

    // What this module extends comes first, and is folded exactly as an import
    // is -- dependency-first, so a chain of 'extends' transits. First, so that a
    // name the base has and something of this module's own also brings in is
    // reported at what this module wrote, which is the thing to change
    INode **nodesp;
    uint32_t cnt;
    ImportNode *extends = mod->extends;
    if (extends) {
        if (extends->module->foldstate == 1)
            extends->cycle = modFoldCycle(extends);
        else {
            nodesAdd(&foldimps, (INode*)extends);
            modFoldNames(pstate, extends->module);
            --foldimps->used;
        }
        importNameRes(pstate, extends);
    }

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
