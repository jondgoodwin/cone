/** Module and import node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <stdio.h>
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
    mod->foldpass = 0;
    mod->folding = 0;
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

// Do two bindings stand for the same thing: the same declaration, reached
// through the same global, if any?
int modFoldSameBinding(INode *a, INode *b) {
    INode *dcl = modBindingDcl(a);
    return dcl != NULL && dcl == modBindingDcl(b) && aliasDclThrough(a) == aliasDclThrough(b);
}

// Did the module's own source write this binding under its name? Everything but
// what a star clause made: a declaration, a typedef, the name an import binds to
// its module, a listed item of a clause, and an enum's 'use'
static int modBindingWritten(INode *node) {
    return !(node->tag == AliasDclTag && (node->flags & FlagUnlisted));
}

// The state of the fold passes (modFoldAll)
static uint16_t foldpass = 0;   // The current pass, stamped on each module it reaches
static int foldreporting = 0;   // This pass reports what could not be folded
static int foldcycled = 0;      // This pass reached a module whose folds were running
static int foldwaited = 0;      // A fold waited in this pass for a name to arrive
static int foldprogress = 0;    // This pass bound a name, or made a binding travel further

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
//
// It is also what lets the fold passes stop (modFoldAll): a pass re-binding what
// an earlier one bound changes nothing here, so a pass that binds no new name,
// and makes no binding public or a fold, has reached the fixpoint. A written
// duplicate is met once, whatever the passes: a listed item, a global's clause
// and an enum's 'use' are each made once, and a star clause's items are never
// written.
INode *modFoldBind(ModuleNode *mod, AliasDclNode *alias) {
    INode *prior = namespaceAdd(&mod->namespace, alias->namesym, (INode*)alias);
    if (prior == NULL) {
        nametblHookNode(alias->namesym, (INode*)alias);
        foldprogress = 1;
        return NULL;
    }
    if (!modFoldSameBinding((INode*)alias, prior))
        return prior;
    if (modBindingWritten(prior) && modBindingWritten((INode*)alias))
        return prior;
    if (prior->tag == AliasDclTag) {
        uint16_t travels = prior->flags & (FlagPub | FlagImportName);
        if (alias->flags & FlagPub)
            prior->flags |= FlagPub;
        if (modBindingWritten((INode*)alias))
            prior->flags &= 0xffff - FlagUnlisted;
        prior->flags &= 0xffff - FlagImportName;
        // Public now, or a fold now: it travels further than it did, and a
        // module folding from this one has more to take in the next pass.
        // Becoming written changes nothing another module can take
        if ((prior->flags & (FlagPub | FlagImportName)) != travels)
            foldprogress = 1;
    }
    return NULL;
}

// Report the binding modFoldBind found holding a fold's name, at 'at' -- where
// a single pass would have met it (modFoldCollisionAt). Where both stand for the
// same declaration, the module wrote one thing twice; otherwise the name means
// two things
void modFoldDupReport(INode *at, AliasDclNode *alias, INode *prior) {
    if (modFoldSameBinding((INode*)alias, prior))
        errorMsgNode(at, ErrorDupName,
            "%s is already a name of this module, for the same thing. A module brings a name in one way: leave out the second.",
            &alias->namesym->namestr);
    else
        errorMsgNode(at, ErrorDupName,
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
    // star leaves out what the base's imports bind (importStarAdmits)
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

// ---- The fold passes -------------------------------------------------------
//
// Every name a module holds by FOLDING is put in its namespace before ANY
// module's own nodes are name resolved. What a module holds by folding used to
// arrive at the start of that module's own resolution, and modules were resolved
// in the order they were loaded -- so a module loaded before the one that folded
// a name could not reach it through the qualifier and a module loaded after
// could. That was load order deciding what a name means, which is not something
// a program may depend on.
//
// A fold reads another module's namespace, and what it reads must be complete:
// a name that module re-exports is there only once its own folds have run. So
// modFoldNames is DEPENDENCY-FIRST -- what a module extends and what it imports
// are folded before it reads them -- and that is what makes transit work: what
// one module re-exported is what the next one finds. Where the imports form no
// cycle, one pass is the whole of it.
//
// A cycle of imports is legal, and there dependency-first cannot be had: where A
// and B import each other, one of them reads the other while the other's folds
// are still running, and a name the other re-exports is not there yet. So the
// passes are REPEATED until one binds nothing new -- a fixpoint, the way Rust
// resolves glob imports -- and a re-export travels round a cycle as it travels
// anywhere else. What makes that cheap is modFoldBind: two bindings of the same
// declaration under one name are one binding, so a pass re-binding what an
// earlier pass bound changes nothing, and "nothing new" is the whole test. A
// namespace only grows, and a binding only becomes more visible, so it ends.
//
// Until then, nothing a later pass might bring is reported missing. A listed
// name the source has not got, or has only privately, and a global's fold or an
// enum's 'use' whose source is named through a binding not yet there, WAIT
// (modFoldWait). The pass after the fixpoint REPORTS: it reads nothing afresh,
// and each fold still waiting reports why it could not be made. A collision --
// two different declarations under one name -- is final the moment it is met,
// since neither binding will ever leave, so it is reported then, once, at the
// fold a single pass would have met it at (modFoldCollisionAt).

// Is this the pass that reports what could not be folded?
int modFoldReporting() {
    return foldreporting;
}

// Record that a fold could not be made in this pass and waits for a later one
void modFoldWait() {
    foldwaited = 1;
}

// The declaration a source names, as far as it can be told before the source is
// resolved: NULL where a lookup at some step finds nothing bound, and the node
// itself wherever there is nothing a fold pass could still bring -- something
// other than a name, or a path through something other than a module
static INode *modFoldPeek(INode *node) {
    if (node == NULL)
        return node;
    if (isNameUseNode(node)) {
        NameUseNode *name = (NameUseNode*)node;
        // The module's namespace is hooked while its folds run, so a bare name
        // is found where resolving it would find it
        return name->dclnode ? name->dclnode : name->namesym->node;
    }
    if (node->tag == RefTag || node->tag == VirtRefTag)
        return modFoldPeek(((RefNode*)node)->vtexp);
    if (node->tag == PtrTag)
        return modFoldPeek(((StarNode*)node)->vtexp);
    if (node->tag != FnCallTag)
        return node;
    FnCallNode *path = (FnCallNode*)node;
    if (path->methfld == NULL || !isNameUseNode(path->methfld) || (path->flags & FlagOperator))
        return node;
    INode *base = modFoldPeek(path->objfn);
    if (base == NULL)
        return NULL;
    base = modBindingDcl(base);
    if (base == NULL || base->tag != ModuleTag)
        return node;
    return namespaceFind(&((ModuleNode*)base)->namespace, ((NameUseNode*)path->methfld)->namesym);
}

// Would this source name nothing yet? (module.h)
int modFoldAwaits(INode *source) {
    return modFoldPeek(source) == NULL;
}

// Is 'unit' this fold of a module, or did this fold make the binding 'made'?
static int modFoldIs(INode *fold, FoldClause *clause, INode *unit, INode *made) {
    if (unit)
        return fold == unit;
    if (clause == NULL)
        return 0;
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(clause->items, cnt, itemp))
        if (*itemp == made)
            return 1;
    return 0;
}

// The place of a fold in the order modFoldNames runs a module's folds, found as
// 'unit' itself or as the fold whose items hold 'made'; -1 for none, which is
// what a binding no fold made gets: a declaration, or an import's own name
static int modFoldPlace(ModuleNode *mod, INode *unit, INode *made) {
    int place = 0;
    INode **nodesp;
    uint32_t cnt;
    if (mod->extends) {
        if (modFoldIs((INode*)mod->extends, mod->extends->fold, unit, made))
            return place;
        ++place;
    }
    for (nodesFor(mod->imports, cnt, nodesp)) {
        if (modFoldIs(*nodesp, ((ImportNode*)*nodesp)->fold, unit, made))
            return place;
        ++place;
    }
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if ((*nodesp)->tag != VarDclTag || ((VarDclNode*)*nodesp)->fold == NULL)
            continue;
        if (modFoldIs(*nodesp, ((VarDclNode*)*nodesp)->fold, unit, made))
            return place;
        ++place;
    }
    for (nodesFor(mod->enumuses, cnt, nodesp)) {
        if (modFoldIs(*nodesp, ((EnumUseNode*)*nodesp)->fold, unit, made))
            return place;
        ++place;
    }
    return -1;
}

// Where to report a fold's binding colliding with what already holds the name
// (module.h). Asked only once a collision is met, so the walk costs nothing
// where a program has none
INode *modFoldCollisionAt(ModuleNode *mod, INode *unit, INode *alias, INode *prior) {
    if (prior->tag != AliasDclTag)
        return alias;
    return modFoldPlace(mod, NULL, prior) > modFoldPlace(mod, unit, NULL) ? prior : alias;
}

// Run one module's folds for the current pass: what it extends first, then its
// imports, its globals' 'use' clauses and its enum 'use's, each in the order
// written. Each fold it reads from is run first, and a module reached again while
// its folds are running -- a cycle -- is read as far as it has got, and read
// again in the next pass.
void modFoldNames(NameResState *pstate, ModuleNode *mod) {
    if (mod->foldpass == foldpass) {
        if (mod->folding)
            foldcycled = 1;
        return;
    }
    mod->foldpass = foldpass;
    mod->folding = 1;

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
        modFoldNames(pstate, extends->module);
        importNameRes(pstate, extends);
    }

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
    // the order they were written in. Each such global is resolved here, once,
    // and left out of the walk in modNameRes. Its type named through a binding
    // not there yet waits for a later pass rather than being reported missing.
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if ((*nodesp)->tag != VarDclTag || ((VarDclNode*)*nodesp)->fold == NULL)
            continue;
        VarDclNode *global = (VarDclNode*)*nodesp;
        if (global->fold->expanded)
            continue;
        if (!foldreporting && modFoldAwaits(global->vtype)) {
            foldwaited = 1;
            continue;
        }
        global->fold->expanded = 1;
        inodeNameRes(pstate, nodesp);
        foldGlobalExpand(pstate, mod, global);
    }

    // A 'use' of an enum folds its variants in as names of this module. Last,
    // so the enum may be named through anything the imports and the globals
    // folded in, and before any body resolves, so a variant's bare name is in
    // place wherever it is used
    for (nodesFor(mod->enumuses, cnt, nodesp))
        foldEnumUseExpand(pstate, mod, (EnumUseNode*)*nodesp);

    modHook(mod, NULL);
    pstate->mod = owningmod;
    mod->folding = 0;
}

// Fold every module's names into its namespace, pass after pass until one binds
// nothing new, then report what could not be folded (module.h). Another pass is
// needed only where one read a module mid-fold or left a fold waiting; a program
// whose imports form no cycle, and whose folds all find what they name, takes one
// pass and the report.
void modFoldAll(NameResState *pstate, Nodes *modules) {
    INode **nodesp;
    uint32_t cnt;
    do {
        ++foldpass;
        foldcycled = foldwaited = foldprogress = 0;
        for (nodesFor(modules, cnt, nodesp))
            modFoldNames(pstate, (ModuleNode*)*nodesp);
    } while (foldprogress && (foldcycled || foldwaited));

    ++foldpass;
    foldreporting = 1;
    for (nodesFor(modules, cnt, nodesp))
        modFoldNames(pstate, (ModuleNode*)*nodesp);
    foldreporting = 0;
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
