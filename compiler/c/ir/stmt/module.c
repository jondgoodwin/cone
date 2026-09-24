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
    mod->moduses = newNodes(4);
    mod->nodes = newNodes(64);
    namespaceInit(&mod->namespace, 64);
    dclInfoInit(&mod->dclinfo);
    mod->foldpass = 0;
    mod->folding = 0;
    mod->dagmark = 0;
    mod->extendsname = NULL;
    mod->extends = NULL;
    mod->deffold = NULL;
    mod->traitname = NULL;
    mod->trait = NULL;
    mod->ntaken = 0;
    mod->initfn = NULL;
    mod->finalfn = NULL;
    mod->genericinfo = NULL;
    mod->generic = NULL;
    mod->instdeps = NULL;
    return mod;
}

// What a name the compiler binds before any source is read stands for, in the
// words a diagnostic names it with, or NULL for a declaration a source wrote.
// These are the names stdlibInit binds and never hooks: no source declares a
// number type or a permission
static char *modBuiltinKind(INode *node) {
    switch (node->tag) {
    case PermTag: return "a built-in permission";
    case IntNbrTag: case UintNbrTag: case FloatNbrTag: return "a built-in type";
    default:
        if (node == (INode*)initAllFn || node == (INode*)finalAllFn)
            return "a built-in function";
        return NULL;
    }
}

// Add a newly parsed named node to the module:
// - We hook all names in global name table at parse time to check for name dupes and
//     because permissions and allocators do not support forward references
// - We remember all public names for later resolution of qualified names
void modAddNamedNode(ModuleNode *mod, Name *name, INode *node) {

    // A word held for a feature not built yet, which a folder or a file named
    // a module after, since no name token reads as one. Reported and released
    // as lexScanIdent reports and releases one it reads, so the rest of the
    // compile, the 'mod' line restating it included, takes it as the name meant
    if (name->node && name->node->tag == KeywordTag && name->node->flags == ReservedToken) {
        errorMsgNode(node, ErrorReserved,
            "'%s' is reserved for a language feature that is not implemented yet. Rename it.", &name->namestr);
        name->node = NULL;
    }

    // Hook into global name table (and add to namednodes), if not already there
    if (!name->node) {
        nametblHookNode(name, (INode*)node);
        namespaceSet(&mod->namespace, name, node);
        return;
    }
    // A built-in has no place in any source to point at, so the one diagnostic
    // is the declaration's, and names what the name already is. A keyword's
    // node is a bare sentinel with no position at all (keyAdd); a folder or a
    // file names a module without a name token, which is how one gets here
    char *builtin = modBuiltinKind(name->node);
    if (name->node->tag == KeywordTag)
        errorMsgNode((INode *)node, ErrorDupName,
            "%s is a keyword, and a keyword cannot be a name. Choose another.", &name->namestr);
    else if (builtin)
        errorMsgNode((INode *)node, ErrorDupName,
            "%s is already the name of %s. A global name must be unique, so choose another.",
            &name->namestr, builtin);
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
    // A standalone 'use' is neither a declaration nor a field: it declares
    // bindings, and those are made in the fold pass, so it is held beside the
    // imports and never joins the walks
    if (node->tag == ModUseTag) {
        nodesAdd(&mod->moduses, node);
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
    if (mod->genericinfo)
        genericInfoPrint(mod->genericinfo);
    // An instance of a generic module: its type arguments
    if (mod->generic) {
        Nodes *args = ((FnCallNode*)mod->instnode)->args;
        INode **argsp;
        uint32_t argcnt;
        inodeFprint("[");
        for (nodesFor(args, argcnt, argsp)) {
            inodePrintNode(*argsp);
            if (argcnt > 1)
                inodeFprint(", ");
        }
        inodeFprint("]");
    }
    if (mod->extendsname)
        inodeFprint(" extends %s", &((NameUseNode*)mod->extendsname)->namesym->namestr);
    if (mod->traitname)
        inodeFprint(" is %s", &((NameUseNode*)mod->traitname)->namesym->namestr);
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
// (works equally well from parent to child or child to parent).
//
// The new module's names REPLACE whatever is hooked, rather than layering over
// it: a module's namespace holds what it declares, what its own folds and
// imports brought in, and its children, and nothing reaches it from the scope
// it was entered from. That matters wherever a module is entered while another's
// names are hooked -- a fold pass run dependency-first from inside another
// module's (modFoldNames), a type resolved by demand from another module's
// (structNameResDemand) -- where layering would let the outer module's names
// answer a lookup the inner module's own namespace cannot. So a module takes two
// hooktables: one hiding everything hooked beneath it, and one over that holding
// the module's own names, which is the one a fold's binding joins (modFoldBind).
void modHook(ModuleNode *oldmod, ModuleNode *newmod) {
    if (oldmod) {
        nametblHookPop();
        nametblHookPop();
    }
    if (newmod) {
        nametblHookPush();
        nametblHookHideBelow();
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
    if (found != NULL && (found->tag == ModTraitTag || (found->tag == StructTag && (found->flags & TraitType)))) {
        errorMsgNode((INode*)name, ErrorModExtends,
            "%s is a trait. A module's 'extends' reuses a concrete module; a module conforms to a module trait with 'is': 'mod name is %s'.",
            &name->namesym->namestr, &name->namesym->namestr);
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
    // A generic module's names belong to each instance, and 'extends' names a
    // module by one name, which cannot carry type arguments
    if (base->genericinfo) {
        errorMsgNode((INode*)name, ErrorGenModBare,
            "%s is a generic module, whose names belong to each instance: a module cannot extend it.",
            &name->namesym->namestr);
        return;
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
// one module re-exported is what the next one finds. Imports form a DAG, a loop
// being refused before the folds run (pgmModuleOrder), so dependency-first can
// always be had.
//
// One pass is not always the whole of it, though. Within one module the folds
// run in a fixed order -- 'extends', the imports, the globals, the standalone
// 'use's -- so a global whose type a later 'use' of a submodule brings WAITS, and
// is folded in the next pass; a sister that took the module's names in the first
// pass takes the global's re-export in the second. So the passes are REPEATED
// until one binds nothing new -- a fixpoint, the way Rust resolves glob imports.
// What makes that cheap is modFoldBind: two bindings of the same declaration
// under one name are one binding, so a pass re-binding what an earlier pass bound
// changes nothing, and "nothing new" is the whole test. A namespace only grows,
// and a binding only becomes more visible, so it ends. The same passes are what
// keep a refused loop to one diagnostic: the names still travel round it, so
// nothing the loop cut short is reported missing as well.
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
// 'unit' itself or as the fold whose items hold 'made', or as the import that
// bound a name of the parent; -1 for none, which is what a binding no fold made
// gets: a declaration, or an import's own name for its module
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
        ImportNode *import = (ImportNode*)*nodesp;
        if (modFoldIs(*nodesp, import->fold, unit, made)
            || (made != NULL && made == (INode*)import->binding))
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
    for (nodesFor(mod->moduses, cnt, nodesp)) {
        ModUseNode *use = (ModUseNode*)*nodesp;
        // A submodule's fold reports as its import-shaped fold, which shares the
        // statement's clause
        if (modFoldIs(*nodesp, use->fold, unit, made)
            || (unit != NULL && unit == (INode*)use->modfold))
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

// Check a module's DEFAULT FOLD -- the 'use' on its 'mod' line, which says what a
// bare import of the module folds [Jon 23 Sep] -- against the module's own
// namespace. Run in the pass that reports, after the module's own folds, so a
// name it re-exports counts as one of its public names. Every name the clause
// lists or leaves out with 'but' must be a public name of the module: the clause
// chooses among what 'pub' already made reachable, and is no export list of its
// own. What it cannot fold is reported here, once, at the 'mod' line, and an
// import's copy of the clause passes it over (importDefaultFold).
static void modDefaultFoldCheck(ModuleNode *mod) {
    FoldClause *fold = mod->deffold;
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(fold->items, cnt, itemp)) {
        Name *name = ((AliasDclNode*)*itemp)->namesym;
        // Listed twice is written twice, which every clause refuses
        INode **priorp = (INode**)(fold->items + 1);
        while (priorp < itemp && ((AliasDclNode*)*priorp)->namesym != name)
            ++priorp;
        INode *found = namespaceFind(&mod->namespace, name);
        if (priorp < itemp)
            errorMsgNode(*itemp, ErrorDupName,
                "%s is listed already. A 'mod' line's 'use' names each default fold once: leave out the second.",
                &name->namestr);
        else if (found == NULL)
            errorMsgNode(*itemp, ErrorNoMbr, "%s has no name %s to fold in.",
                &mod->namesym->namestr, &name->namestr);
        else if (found == (INode*)mod)
            errorMsgNode(*itemp, ErrorBadFold,
                "%s is this module's own name, and an import of the module binds it already.",
                &name->namestr);
        else if (inodeIsPrivate(found))
            errorMsgNode(*itemp, ErrorNotPublic, "%s is private to %s, so it cannot be a default fold.",
                &name->namestr, &mod->namesym->namestr);
    }
    if (fold->excludes == NULL)
        return;
    for (nodesFor(fold->excludes, cnt, itemp)) {
        Name *name = ((NameUseNode*)*itemp)->namesym;
        INode *found = namespaceFind(&mod->namespace, name);
        if (found == NULL)
            errorMsgNode(*itemp, ErrorNoMbr, "%s has no member named %s to leave out.",
                &mod->namesym->namestr, &name->namestr);
        else if (inodeIsPrivate(found))
            errorMsgNode(*itemp, ErrorNotPublic, "%s is private to %s, so '*' never folds it; there is nothing to leave out.",
                &name->namestr, &mod->namesym->namestr);
    }
}

// Run one module's folds for the current pass: what it extends first, then its
// imports, its globals' 'use' clauses and its standalone 'use's, each in the order
// written. Each fold it reads from is run first. A module reached again while its
// folds are running closes a loop, which pgmModuleOrder has refused already: it is
// read as far as it has got, and read again in the next pass.
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
        // An import of a name of the parent reads the parent's namespace, so
        // the parent's folds come first. Where the name binds a module, the
        // import folds from it as from any module, below
        if (import->binding) {
            modFoldNames(pstate, (ModuleNode*)mod->dclinfo.owner);
            importBindName(mod, import);
        }
        else if (import->isnamedfile)
            importBindName(mod, import);
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

    // A standalone 'use' folds an enum's variants or a submodule's public names
    // in as names of this module. Last, so the enum may be named through
    // anything the imports and the globals folded in, and before any body
    // resolves, so a folded bare name is in place wherever it is used
    for (nodesFor(mod->moduses, cnt, nodesp))
        foldModUseExpand(pstate, mod, (ModUseNode*)*nodesp);

    // What the module's own 'mod' line says a bare import of it folds is judged
    // against the module once its namespace is complete
    if (foldreporting && mod->deffold)
        modDefaultFoldCheck(mod);

    // The module's namespace is complete, and nothing folding from this module
    // has read it yet, so this is where it takes its module trait's defaults:
    // a module extending it, or importing it with 'use *', then takes them as
    // this module's own declarations (modTraitConform)
    modTraitConform(pstate, mod, 0);

    modHook(mod, NULL);
    pstate->mod = owningmod;
    mod->folding = 0;
}

// Fold every module's names into its namespace, pass after pass until one binds
// nothing new, then report what could not be folded (module.h). Another pass is
// needed only where one left a fold waiting, or read a module mid-fold round a
// loop already refused; a program whose folds all find what they name takes one
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

// ---- Generic modules: 'mod stack[T];' ---------------------------------------
//
// A generic module is treated as a generic type is [Jon 23 Sep: "if I treated
// generic modules like I treated generic types, how would I treat them?"]. It
// is written with its type parameters in square brackets on its 'mod' line,
// resolved once in place with them hooked, and never type checked or generated
// itself. 'stack[i64]' names an INSTANCE, made the first time those arguments
// are named and memoized on the generic (genericMemoize): the same arguments
// anywhere in the program are one instance, with one set of globals. An
// instance is a module of its own -- its own globals, its own 'init' and
// 'final' -- cloned from the generic with each parameter substituted, as a
// generic type's instance is cloned from the type.

// Refuse what a generic module holds that an instance cannot yet be cloned with.
// Its functions, globals, plain types, overload names and type aliases are
// cloned; a generic function or type inside it would need its own parameters
// carried through the clone, and a trait, an enum, a macro, a module trait or a
// global's 'use' clause each keep bindings the clone does not re-point.
static void modGenericCheckBody(ModuleNode *mod) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        INode *node = *nodesp;
        char *what = NULL;
        switch (node->tag) {
        case FnDclTag:
            if (((FnDclNode*)node)->genericinfo)
                what = "a generic function";
            break;
        case VarDclTag:
            if (((VarDclNode*)node)->fold)
                what = "a global with a 'use' clause";
            break;
        case StructTag:
        {
            StructNode *strnode = (StructNode*)node;
            if (strnode->genericinfo)
                what = "a generic type";
            else if (strnode->basetrait)
                continue;   // a variant: its enum or trait is reported
            else if (node->flags & TraitType)
                what = "a trait or an enum";
            break;
        }
        case FnOverloadDclTag:
        case AliasDclTag:
            break;
        case MacroDclTag:
            what = "a macro";
            break;
        case ModTraitTag:
            what = "a module trait";
            break;
        default:
            what = "this declaration";
            break;
        }
        if (what)
            errorMsgNode(node, ErrorGenModBody,
                "Generic module %s may not hold %s: an instance of a generic module is not built for one yet.",
                &mod->namesym->namestr, what);
    }
}

// The instances of generic modules made so far, in the order they were made.
// Each is also on its generic's memonodes, as a generic type's instances are;
// this list is what the program adds to its modules once type check is done
// (pgmTypeCheck), since the program's module list is being walked while they
// are made
static Nodes *modInstances = NULL;

Nodes *modInstanceList() {
    return modInstances;
}

// Add to 'deps' the module that declares each named type in 'type': an
// instance follows in the init order the modules its type arguments come from
static void modTypeArgModules(Nodes **deps, INode *type) {
    if (type == NULL)
        return;
    if (isNameUseNode(type))
        type = nameUseGetDcl((NameUseNode*)type);
    if (type == NULL)
        return;
    switch (type->tag) {
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        modTypeArgModules(deps, ((RefNode*)type)->vtexp);
        return;
    case PtrTag:
        modTypeArgModules(deps, ((StarNode*)type)->vtexp);
        return;
    case ArrayTag:
        modTypeArgModules(deps, arrayElemType(type));
        return;
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)type)->elems, cnt, nodesp))
            modTypeArgModules(deps, *nodesp);
        return;
    }
    case AliasDclTag:
        modTypeArgModules(deps, ((AliasDclNode*)type)->target);
        return;
    default:
        break;
    }
    if (inodeGetDclInfo(type) == NULL)
        return;
    ModuleNode *mod = dclInfoGetModule(type);
    if (mod != NULL)
        nodesAdd(deps, (INode*)mod);
}

// Make the instance of a generic module for the type arguments 'srcgencall'
// gives, register it, and type check it (module.h).
//
// The instance is a module of its own: named as the generic is, owned where the
// generic is, and marked with the call that made it (instnode), which is what
// spells its symbols with the type arguments -- 'stack[i64].push' -- as a
// generic type's instance is spelled. Its imports are the generic's: they were
// bound and folded once, and what the generic depends on the instance depends on.
//
// Its declarations are cloned the way a generic type's members are
// (cloneStructNode): a shell for each first, bound in the instance's namespace
// and mapped from the generic's declaration, and only then each signature, body,
// type and initial value -- so a body naming another declaration of the module,
// before or after it, names the instance's. The generic's own name is mapped to
// the instance too, so 'stack.count' inside the generic means this instance's
// global; given type arguments it names the generic again (cloneFnCallNode). A
// use of a type parameter becomes a copy of the argument (cloneNode). Every
// other name the generic's namespace binds -- what its imports bound and folded
// -- is bound to the same declaration in the instance's.
//
// It is registered on the generic's memonodes BEFORE it is type checked, as a
// generic type's instance is, so a body naming 'stack[i64]' inside the instance
// reaches it rather than instantiating it again.
ModuleNode *modInstantiate(TypeCheckState *pstate, FnCallNode *srcgencall, ModuleNode *generic) {
    GenericInfo *geninfo = generic->genericinfo;
    ModuleNode *inst = newModuleNode();
    inodeLexCopy((INode*)inst, (INode*)generic);
    inst->namesym = generic->namesym;
    inst->filesym = generic->filesym;
    inst->foldersym = generic->foldersym;
    inst->dclinfo = generic->dclinfo;
    inst->flags = (generic->flags & (FlagModDcl | FlagPub)) | FlagGenMod | NameResolved;
    inst->instnode = (INode*)srcgencall;
    inst->generic = generic;
    inst->imports = generic->imports;
    inst->traitname = generic->traitname;
    inst->trait = generic->trait;
    inst->ntaken = generic->ntaken;
    inst->instdeps = newNodes(2);

    CloneState cstate;
    clonePushState(&cstate, (INode*)srcgencall, NULL, 0, geninfo->parms, srcgencall->args);
    uint32_t dclpos = cloneDclPush();
    cloneDclSetMap((INode*)generic, (INode*)inst);

    // A shell for every declaration, mapped from the generic's, before anything
    // is copied into any of them
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(generic->nodes, cnt, nodesp)) {
        INode *node = *nodesp;
        INode *copy;
        switch (node->tag) {
        case FnDclTag:
            copy = (INode*)cloneFnDclShell((FnDclNode*)node);
            break;
        case VarDclTag:
            copy = (INode*)cloneVarDclShell((VarDclNode*)node);
            break;
        case StructTag:
            // Filled by cloneStructNode, which takes a reserved shell
            copy = memAllocBlk(sizeof(StructNode));
            break;
        case FnOverloadDclTag:
        {
            FnOverloadDclNode *ovl = newFnOverloadDclNode(((FnOverloadDclNode*)node)->namesym);
            inodeLexCopy((INode*)ovl, node);
            copy = (INode*)ovl;
            break;
        }
        case AliasDclTag:
            copy = memAllocBlk(sizeof(AliasDclNode));
            memcpy(copy, node, sizeof(AliasDclNode));
            break;
        default:
            // modGenericCheckBody refused it, and a compile with errors at
            // name resolution never reaches type check
            errorUnreachable(node, "a declaration a generic module's instance has no clone for");
            copy = node;
            break;
        }
        copy->instnode = (INode*)srcgencall;
        cloneDclSetMap(node, copy);
        nodesAdd(&inst->nodes, copy);
    }

    // Now every signature, body, type and initial value, in the same order
    INode **copyp = &nodesGet(inst->nodes, 0);
    for (nodesFor(generic->nodes, cnt, nodesp)) {
        INode *node = *nodesp;
        INode *copy = *copyp++;
        switch (node->tag) {
        case FnDclTag:
            cloneFnDclFill(&cstate, (FnDclNode*)copy, (FnDclNode*)node);
            break;
        case VarDclTag:
            cloneVarDclFill(&cstate, (VarDclNode*)copy, (VarDclNode*)node);
            break;
        case StructTag:
            cstate.structshell = copy;
            cloneNode(&cstate, node);
            break;
        case FnOverloadDclTag:
        {
            INode **candp;
            uint32_t candcnt;
            for (nodesFor(((FnOverloadDclNode*)node)->overloads, candcnt, candp))
                fnOverloadDclAdd((FnOverloadDclNode*)copy, (FnDclNode*)cloneDclFix(*candp));
            break;
        }
        case AliasDclTag:
            ((AliasDclNode*)copy)->target = cloneNode(&cstate, ((AliasDclNode*)node)->target);
            break;
        }
        DclInfo *dclinfo = inodeGetDclInfo(copy);
        if (dclinfo)
            dclinfo->owner = (INode*)inst;
    }

    // The instance's namespace: its own declarations where the generic has its,
    // itself under its own name, and every other binding the generic's holds
    Namespace *ns = &generic->namespace;
    namespaceInit(&inst->namespace, ns->avail);
    namespaceFor(ns) {
        NameNode *nn = &ns->namenodes[__i];
        if (nn->name == NULL)
            continue;
        namespaceSet(&inst->namespace, nn->name, cloneDclFix(nn->node));
    }
    cloneDclPop(dclpos);
    clonePopState();

    // Remembered before it is checked, as a generic type's instance is
    if (!geninfo->memonodes)
        geninfo->memonodes = newNodes(2);
    nodesAdd(&geninfo->memonodes, (INode*)srcgencall);
    nodesAdd(&geninfo->memonodes, (INode*)inst);
    if (modInstances == NULL)
        modInstances = newNodes(4);
    nodesAdd(&modInstances, (INode*)inst);

    // Its place in the init order: after the generic, which follows what the
    // generic imports, and after the modules its type arguments come from. An
    // instance made while another instance is checked is one that instance
    // uses, so that one follows it (pgmInstanceOrder)
    for (nodesFor(srcgencall->args, cnt, nodesp))
        modTypeArgModules(&inst->instdeps, *nodesp);
    ModuleNode *user = pstate->fn ? dclInfoGetModule((INode*)pstate->fn) : NULL;
    if (user && user->generic)
        nodesAdd(&user->instdeps, (INode*)inst);

    // Checked with no function or type around it, as the program's own
    // modules are, whichever function's body named it
    TypeCheckState tstate;
    tstate.typenode = NULL;
    tstate.fn = NULL;
    tstate.scope = 0;
    INode *instnode = (INode*)inst;
    inodeTypeCheckAny(&tstate, &instnode);
    return inst;
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

    // A generic module is resolved once, in place, as a generic type is: its
    // type parameters are hooked over its own names, and every use of one in its
    // body binds to the parameter. An instance is never resolved: it is a clone,
    // which re-points each use at the instance's argument (modInstantiate)
    if (mod->genericinfo) {
        nametblHookPush();
        for (nodesFor(mod->genericinfo->parms, cnt, nodesp))
            inodeNameRes(pstate, nodesp);
        modGenericCheckBody(mod);
    }

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

    // The copies of a module trait's defaults are the last 'ntaken' nodes, and
    // arrived resolved in the trait's scope (modTraitConform): a walk of them
    // here would bind their names a second time, in the wrong scope
    uint32_t own = mod->nodes->used - mod->ntaken;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if (own-- == 0)
            break;
        // Resolved by one of the passes above
        if ((*nodesp)->tag == VarDclTag && ((VarDclNode*)*nodesp)->fold != NULL)
            continue;
        if ((*nodesp)->tag == AliasDclTag && ((*nodesp)->flags & FlagTypeAlias))
            continue;
        inodeNameRes(pstate, nodesp);
    }
    mod->flags = (mod->flags & ~NameResolving) | NameResolved;
    if (mod->genericinfo)
        nametblHookPop();

    // Switch name table back to owner module
    modHook(mod, NULL);
    pstate->mod = owningmod;
}

static void modLifecycle(ModuleNode *mod);

// Type check the module node
void modTypeCheck(TypeCheckState *pstate, ModuleNode *mod) {
    INode **nodesp;
    uint32_t cnt;

    // Type check any imported modules this module depends on first
    for (nodesFor(mod->imports, cnt, nodesp)) {
        inodeTypeCheckAny(pstate, nodesp);
    }

    // A generic module is never checked, as a generic type is not: its body is
    // written against its type parameters, which stand for nothing. Each
    // instance is checked as it is made (modInstantiate)
    if (mod->genericinfo)
        return;

    // What the module declares for each member of the module trait it
    // conforms to has the member's shape, checked where 'is' is written
    modTraitCheck(pstate, mod);

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

    // Last, once every global's type is settled: the module's 'init' and 'final',
    // and the 'drop' that finalizes its globals
    modLifecycle(mod);
}

// A global the module declares without an initial value, which its 'init' must
// assign. An 'extern' global is defined elsewhere, initial value and all
static int modGlobalUninit(INode *node) {
    return node->tag == VarDclTag && ((VarDclNode*)node)->value == NULL
        && !(((VarDclNode*)node)->dclinfo.facts & DclExternal);
}

ModuleNode *modInitOf(FnDclNode *fnnode) {
    if (fnnode->namesym != initName || (fnnode->flags & FlagMethFld))
        return NULL;
    INode *owner = fnnode->dclinfo.owner;
    return owner != NULL && owner->tag == ModuleTag ? (ModuleNode*)owner : NULL;
}

uint16_t *modInitFlowBegin(ModuleNode *mod) {
    uint16_t *saved = (uint16_t*)memAllocBlk(sizeof(uint16_t) * (mod->nodes->used + 1));
    INode **nodesp;
    uint32_t cnt;
    uint32_t pos = 0;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if (modGlobalUninit(*nodesp)) {
            VarDclNode *var = (VarDclNode*)*nodesp;
            saved[pos] = var->flowtempflags;
            var->flowtempflags &= 0xFFFF - (VarInitialized | VarMoved);
        }
        ++pos;
    }
    return saved;
}

void modInitFlowEnd(ModuleNode *mod, uint16_t *saved) {
    INode **nodesp;
    uint32_t cnt;
    uint32_t pos = 0;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if (modGlobalUninit(*nodesp)) {
            VarDclNode *var = (VarDclNode*)*nodesp;
            if (!(var->flowtempflags & VarInitialized))
                errorMsgNode((INode*)var, ErrorGlobalUninit,
                    "Global %s has no initial value, and its module's init never assigns it one.",
                    &var->namesym->namestr);
            var->flowtempflags = saved[pos];
        }
        ++pos;
    }
}

// Find the module's own declaration of 'init' or 'final', and check that it is
// declared as the one the program's stitched init or final calls:
// 'fn @initpure init()' or 'fn final()', a function of no parameters returning
// nothing, with a symbol of its own. NULL where the module declares none -- a
// name its folds brought is another module's -- or where it is malformed,
// which is reported.
static FnDclNode *modLifecycleFn(ModuleNode *mod, Name *name) {
    INode *dcl = namespaceFind(&mod->namespace, name);
    if (dcl == NULL || dcl->tag == AliasDclTag || dcl->tag == ModuleTag)
        return NULL;
    char *form = name == initName ? "fn @initpure init()" : "fn final()";
    if (dcl->tag != FnDclTag) {
        DclInfo *dclinfo = inodeGetDclInfo(dcl);
        if (dclinfo != NULL && dclinfo->owner != (INode*)mod)
            return NULL;
        errorMsgNode(dcl, ErrorModLifecycle,
            "A module's %s is its function '%s', which the program runs; it may not name anything else.",
            &name->namestr, form);
        return NULL;
    }
    FnDclNode *fn = (FnDclNode*)dcl;
    if (fn->dclinfo.owner != (INode*)mod)
        return NULL;
    FnSigNode *sig = (FnSigNode*)fn->vtype;
    char *why = NULL;
    if (fn->genericinfo)
        why = "is generic";
    else if (fn->overloadsym)
        why = "declares an overload name";
    else if (fn->flags & FlagInline)
        why = "is inline, which leaves no function for the program's stitched init and final to call";
    else if (sig->parms->used != 0)
        why = "takes parameters";
    else if (itypeGetTypeDcl(sig->rettype)->tag != VoidTag)
        why = "returns a value";
    else if (name == initName && !(fn->dclinfo.facts & DclInitPure))
        why = "is not marked '@initpure'";
    if (why) {
        errorMsgNode((INode*)fn, ErrorModLifecycle, "A module's %s is declared '%s', and this one %s.",
            &name->namestr, form, why);
        return NULL;
    }
    fn->dclinfo.facts |= DclLifecycle;
    return fn;
}

// Give the module a 'drop' where any global it declares needs finalizing: a
// function owned by the module, so its symbol is spelled after it ('q.drop'),
// that calls the module's own 'final' and then each such global's drop function,
// in the order the globals are declared -- a type's 'drop' is its 'final' and
// then its fields', and a module's globals are its fields. Built pre-lowered, as
// a type's is, and never type checked or flow analyzed. A C-named global is C's
// storage and not finalized. Where no global needs it, the module's finalizer is
// its own 'final', or it has none.
static FnDclNode *modGiveDrop(ModuleNode *mod, FnDclNode *final) {
    BlockNode *block = NULL;
    FnDclNode *dropfn = NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if ((*nodesp)->tag != VarDclTag)
            continue;
        VarDclNode *var = (VarDclNode*)*nodesp;
        if (var->dclinfo.facts & DclCName)
            continue;
        INode *vardrop = itypeGetDropFnDcl(var->vtype);
        if (vardrop == NULL)
            continue;

        if (block == NULL) {
            // The name is the module's finalizer's symbol: a 'drop' the module
            // declares itself would be spelled the same
            INode *prior = namespaceFind(&mod->namespace, dropName);
            if (prior != NULL && prior->tag != AliasDclTag && prior->tag != ModuleTag) {
                errorMsgNode(prior, ErrorModLifecycle,
                    "A module whose globals need finalizing is given a finalizer named drop, so the module may not declare a drop of its own.");
                return NULL;
            }
            FnSigNode *fnsig = newFnSigNode();
            fnsig->rettype = (INode*)newVoidNode();
            block = newBlockNode();
            dropfn = newFnDclNode(dropName, 0, (INode*)fnsig, (INode*)block);
            inodeLexCopy((INode*)dropfn, (INode*)mod);
            dclInfoJoin((INode*)dropfn, (INode*)mod);
            dropfn->dclinfo.facts |= DclLifecycle;
            dropfn->flags |= TypeChecked;
            if (final) {
                FnCallNode *finalcall = newFnCallLower((INode*)var, (INode*)final, 1);
                nodesAdd(&block->stmts, (INode*)finalcall);
            }
        }

        FnCallNode *dropcall = newFnCallLower((INode*)var, vardrop, 1);
        INode *varuse = newNameUseFromDclNode((INode*)var, (INode*)var);
        nodesAdd(&dropcall->args, newBorrowMutRef(varuse, var->vtype, (INode*)uniPerm));
        nodesAdd(&block->stmts, (INode*)dropcall);
    }
    if (block == NULL)
        return final;

    BreakRetNode *retnode = newReturnNode();
    retnode->exp = (INode*)newNilLitNode();
    retnode->block = block;
    nodesAdd(&block->stmts, (INode*)retnode);
    // Among the module's nodes, so it is named and generated wherever the
    // module is; the module trait's copies were counted from the end of 'nodes'
    // before name resolution, and nothing reads that count after it
    nodesAdd(&mod->nodes, (INode*)dropfn);
    return dropfn;
}

// A module's lifecycle [Jon 23 Sep]: each module declares only its own portion
// -- an 'init' for its own globals, a 'final' for its own state -- and the
// program stitches every module's together (genlStitch). Here: find and check
// the two, report each global without an initial value where there is no 'init'
// to assign it (an 'init' checks its own, modInitFlowEnd), and give the module
// the 'drop' that finalizes its globals after its 'final'.
static void modLifecycle(ModuleNode *mod) {
    mod->initfn = modLifecycleFn(mod, initName);
    FnDclNode *final = modLifecycleFn(mod, finalName);

    // An 'init' the module declares checks its globals itself, well formed or
    // not: one that is malformed has been reported, and that is the one error
    INode *initdcl = namespaceFind(&mod->namespace, initName);
    if (initdcl == NULL || initdcl->tag == AliasDclTag || initdcl->tag == ModuleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(mod->nodes, cnt, nodesp)) {
            if (modGlobalUninit(*nodesp))
                errorMsgNode(*nodesp, ErrorGlobalUninit,
                    "Global %s has no initial value, and its module declares no init to assign it one: write 'fn @initpure init()', or give it a value.",
                    &((VarDclNode*)*nodesp)->namesym->namestr);
        }
    }

    mod->finalfn = modGiveDrop(mod, final);
}
