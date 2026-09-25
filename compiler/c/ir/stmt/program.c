/** Program node helper routines
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

// Create a new Program node
ProgramNode *newProgramNode() {
    ProgramNode *pgm;
    newNode(pgm, ProgramNode, ProgramTag);
    pgm->modules = newNodes(4);
    pgm->initorder = newNodes(4);
    namespaceInit(&pgm->files, 16);
    return pgm;
}

// Add a new module to the program
ModuleNode *pgmAddMod(ProgramNode *pgm, int16_t flags) {
    ModuleNode *mod = newModuleNode();
    mod->flags |= flags;
    nodesAdd(&pgm->modules, (INode*)mod);
    return mod;
}

// The module a file already belongs to, or NULL if the file has not been read
ModuleNode *pgmFindFile(ProgramNode *pgm, Name *pathsym) {
    return (ModuleNode *)namespaceFind(&pgm->files, pathsym);
}

// Record that a file belongs to a module
void pgmSetFile(ProgramNode *pgm, Name *pathsym, ModuleNode *mod) {
    namespaceSet(&pgm->files, pathsym, (INode*)mod);
}

// Serialize a program node
void pgmPrint(ProgramNode *pgm) {
    inodeFprint("program:\n");
    inodePrintIncr();
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        inodePrintIndent();
        inodePrintNode(*nodesp);
        inodePrintNL();
    }
    inodePrintDecr();
}

// ---- The module order: imports form a DAG ---------------------------------
//
// "Modules need to have an explicit dag order" [Jon 23 Sep]. A module DEPENDS ON:
// - each module it imports, a package, a sister or a file alike;
// - the module holding a name it imports: 'import Point;' in a submodule names
//   its parent's Point, and depends on the parent;
// - the module it extends;
// - each of its own submodules. Containment is a dependency: a program is a
//   hierarchical decomposition, and a part cannot lean on the whole it is part
//   of. So a child that depends on its parent -- by importing a name of it --
//   closes a loop of two.
// One walk over those edges, depth first, places each module after everything it
// depends on. An edge back to a module on the walk's path is a loop, refused and
// named. The walk goes on past it, so every loop is reported once and every
// module still gets a place.

enum {
    DagImport,      // imports the module
    DagImportName,  // imports a name the module holds
    DagExtends,     // extends the module
    DagContains     // holds the module as its submodule
};

// The edge a module on the walk's path is following out of it
typedef struct {
    ModuleNode *mod;
    ModuleNode *to;
    INode *site;    // what wrote the edge; the submodule itself for containment
    Name *name;     // DagImportName: the name imported
    int kind;
} DagStep;

typedef struct {
    ProgramNode *pgm;
    DagStep *path;
    uint32_t depth;
} DagWalk;

// Append to a message buffer, never past its end
static void dagAppend(char *buf, size_t size, const char *fmt, ...) {
    size_t used = strlen(buf);
    if (used + 1 >= size)
        return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + used, size - used, fmt, args);
    va_end(args);
}

// A module as a path names it: its owners' names, then its own
static void dagAppendModule(char *buf, size_t size, ModuleNode *mod) {
    ModuleNode *owner = (ModuleNode*)mod->dclinfo.owner;
    if (owner && owner->tag == ModuleTag) {
        dagAppendModule(buf, size, owner);
        dagAppend(buf, size, ".");
    }
    dagAppend(buf, size, "%s", mod->namesym ? &mod->namesym->namestr : "?");
}

// Report the loop the walk just closed: the path from 'at' to the top, and the
// edge from the top back to 'at'. Reported at the edge that closed it unless that
// is containment, which nothing wrote; then at the first edge round the loop that
// something did write
static void pgmDagLoop(DagWalk *walk, uint32_t at) {
    char buf[2048];
    buf[0] = '\0';
    int contained = 0;
    dagAppend(buf, sizeof(buf), "Import loop between modules: ");
    for (uint32_t i = at; i < walk->depth; ++i) {
        dagAppendModule(buf, sizeof(buf), walk->path[i].mod);
        dagAppend(buf, sizeof(buf), " -> ");
    }
    dagAppendModule(buf, sizeof(buf), walk->path[at].mod);
    dagAppend(buf, sizeof(buf), ". Imports between modules must not loop (");
    for (uint32_t i = at; i < walk->depth; ++i) {
        DagStep *step = &walk->path[i];
        if (i > at)
            dagAppend(buf, sizeof(buf), "; ");
        dagAppendModule(buf, sizeof(buf), step->mod);
        switch (step->kind) {
        case DagImport: dagAppend(buf, sizeof(buf), " imports "); break;
        case DagImportName: dagAppend(buf, sizeof(buf), " imports %s of ", &step->name->namestr); break;
        case DagExtends: dagAppend(buf, sizeof(buf), " extends "); break;
        default: dagAppend(buf, sizeof(buf), " contains "); contained = 1; break;
        }
        dagAppendModule(buf, sizeof(buf), step->to);
        if (step->kind != DagContains)
            dagAppend(buf, sizeof(buf), " at %s:%u", step->site->lexer->url, step->site->linenbr);
    }
    dagAppend(buf, sizeof(buf), ").");
    if (contained)
        dagAppend(buf, sizeof(buf),
            " A module may not depend on a module that contains it: move what they share into a sister both import.");

    DagStep *report = &walk->path[walk->depth - 1];
    for (uint32_t i = at; report->kind == DagContains && i < walk->depth; ++i)
        report = &walk->path[i];
    errorMsgNode(report->site, ErrorImportLoop, "%s", buf);

    // A loop of 'extends' alone would have each module adding to the other, with
    // no surface to start from: cut it where it closed, so the fold passes take
    // neither side. Any other loop is left whole, and the fold passes carry the
    // names round it (modFoldAll), so what the loop alone cut short is not
    // reported as well -- bar a struct a global's fold resolves mid-fold
    // (compiler/c/doc/nodes/module.md, "Hazards")
    int allextends = 1;
    for (uint32_t i = at; i < walk->depth; ++i)
        allextends &= walk->path[i].kind == DagExtends;
    if (allextends)
        walk->path[walk->depth - 1].mod->extends = NULL;
}

static void pgmDagVisit(DagWalk *walk, ModuleNode *mod);

// Follow one edge out of the module at the top of the walk's path
static void pgmDagFollow(DagWalk *walk, ModuleNode *to, int kind, INode *site, Name *name) {
    DagStep *step = &walk->path[walk->depth - 1];
    // An edge to itself is no dependency: it is the prelude's own import of
    // itself, where core is the module a description compiles (a module
    // importing its own file is refused at parse, ErrorModFile)
    if (to == NULL || to->tag != ModuleTag || to == step->mod)
        return;
    step->to = to;
    step->kind = kind;
    step->site = site;
    step->name = name;
    if (to->dagmark == 2)
        return;
    if (to->dagmark == 1) {
        uint32_t at = 0;
        while (walk->path[at].mod != to)
            ++at;
        pgmDagLoop(walk, at);
        return;
    }
    pgmDagVisit(walk, to);
}

// Place everything a module depends on, then the module
static void pgmDagVisit(DagWalk *walk, ModuleNode *mod) {
    mod->dagmark = 1;
    walk->path[walk->depth++].mod = mod;

    if (mod->extends)
        pgmDagFollow(walk, mod->extends->module, DagExtends, mod->extendsname, NULL);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->imports, cnt, nodesp)) {
        ImportNode *import = (ImportNode*)*nodesp;
        if (import->module)
            pgmDagFollow(walk, import->module, DagImport, (INode*)import, NULL);
        else if (import->binding)
            pgmDagFollow(walk, (ModuleNode*)mod->dclinfo.owner, DagImportName, (INode*)import,
                import->binding->namesym);
    }
    for (nodesFor(walk->pgm->modules, cnt, nodesp)) {
        if ((ModuleNode*)((ModuleNode*)*nodesp)->dclinfo.owner == mod)
            pgmDagFollow(walk, (ModuleNode*)*nodesp, DagContains, *nodesp, NULL);
    }

    --walk->depth;
    mod->dagmark = 2;
    nodesAdd(&walk->pgm->initorder, (INode*)mod);
}

// Put the program's modules in dependency order, refusing a loop (program.h)
void pgmModuleOrder(ProgramNode *pgm) {
    DagWalk walk;
    walk.pgm = pgm;
    walk.depth = 0;
    walk.path = (DagStep*)memAllocBlk(sizeof(DagStep) * (pgm->modules->used + 1));
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        if (((ModuleNode*)*nodesp)->dagmark == 0)
            pgmDagVisit(&walk, (ModuleNode*)*nodesp);
    }
}

// Refuse the three places a generic module cannot stand. An executable's root
// has nothing to instantiate it. A submodule of a generic module would be
// instantiated with it, as a generic type's methods are, and that is not built:
// an instance is cloned from its generic's own declarations alone. And a 'mod'
// line's default fold names what an import folds, where a generic's names
// belong to each instance and fold from none.
static void pgmGenericModulesCheck(ProgramNode *pgm) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        ModuleNode *mod = (ModuleNode*)*nodesp;
        ModuleNode *owner = (ModuleNode*)mod->dclinfo.owner;
        if (owner != NULL && owner->tag == ModuleTag && owner->genericinfo)
            errorMsgNode((INode*)mod, ErrorGenModBody,
                "Generic module %s may not hold submodule %s: an instance of a generic module is not built with submodules yet.",
                &owner->namesym->namestr, &mod->namesym->namestr);
        if (mod->genericinfo == NULL)
            continue;
        if (*nodesp == nodesGet(pgm->modules, 0) && !(mod->dclinfo.facts & DclNamesChain))
            errorMsgNode((INode*)mod, ErrorGenModRoot,
                "The program's root module %s is declared generic, and nothing can instantiate a program. A generic module is a submodule, an imported module or a library's root.",
                &mod->namesym->namestr);
        if (mod->deffold)
            errorMsgNode(mod->deffold->at, ErrorGenModBare,
                "%s is a generic module, whose names belong to each instance: a bare import of it can fold nothing, so its 'mod' line names no default fold.",
                &mod->namesym->namestr);
    }
}

// Name resolution of the program node.
//
// Every module's FOLDED names are put in place first, dependency-first, and only
// then is any module's own body resolved. That order is what stops the file load
// order deciding what a qualified name can reach: a fold used to run at the
// start of the folding module's own resolution, so a module resolved earlier --
// the root among them, since it loads first -- looked the name up before it was
// there, and a module resolved later found it.
//
// Ahead of the folds, what each module's 'extends' names is resolved, and then
// the modules are put in dependency order, a loop of imports, 'extends' and
// containment refused (pgmModuleOrder): 'extends' is the one edge not known at
// parse, and the fold follows every edge dependency-first.
//
// A module conforming to a module trait takes the defaults it does not declare
// at the end of its own folds (modTraitConform, from modFoldNames). Between the
// folds and the bodies, any module that could not take them there takes them
// now, and what is wrong with a conformance is reported: after the folds, so a
// name the module holds by folding meets a member as its own declaration does,
// and before any body, so a default the module took is a name of it wherever it
// is named.
void pgmNameRes(NameResState *pstate, ProgramNode *pgm) {
    INode **nodesp;
    uint32_t cnt;
    pgmGenericModulesCheck(pgm);
    for (nodesFor(pgm->modules, cnt, nodesp))
        modExtendsResolve((ModuleNode*)*nodesp);
    pgmModuleOrder(pgm);
    modFoldAll(pstate, pgm->modules);
    for (nodesFor(pgm->modules, cnt, nodesp))
        modTraitConform(pstate, (ModuleNode*)*nodesp, 1);
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        inodeNameRes(pstate, nodesp);
    }
}

// Resolve the names of modules parsed after the program was analysed: the
// include-file generator's self-check, whose first module stands for the root
// and imports what the root imports, and whose others are the modules its
// nested blocks declare. The same steps as pgmNameRes, for them alone: the
// program's modules keep what their own resolution made
void pgmNameResAlone(ProgramNode *pgm, Nodes *mods) {
    NameResState nstate;
    nstate.mod = NULL;
    nstate.typenode = NULL;
    nstate.loopblock = NULL;
    nstate.macromethod = NULL;
    nstate.expander = NULL;
    nstate.sigfn = NULL;
    nstate.scope = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mods, cnt, nodesp))
        modExtendsResolve((ModuleNode*)*nodesp);
    modFoldAlone(&nstate, pgm->modules, mods);
    for (nodesFor(mods, cnt, nodesp))
        modTraitConform(&nstate, (ModuleNode*)*nodesp, 1);
    for (nodesFor(mods, cnt, nodesp))
        inodeNameRes(&nstate, nodesp);
}

// Where a module sits in the init order, or -1 where it has no place yet
static int32_t pgmOrderIndex(ProgramNode *pgm, INode *mod) {
    INode **nodesp;
    uint32_t cnt;
    int32_t pos = 0;
    for (nodesFor(pgm->initorder, cnt, nodesp)) {
        if (*nodesp == mod)
            return pos;
        ++pos;
    }
    return -1;
}

// Give each instance of a generic module its place in the init order, which
// was made at name resolution, before any instance existed (pgmModuleOrder). An
// instance goes straight after the last of what it depends on: its generic,
// which the walk placed after everything the generic imports, the modules its
// type arguments come from, and each instance its own body uses (modInstantiate
// records the last two on 'instdeps'). So it runs its 'init' after theirs, and
// before every module that follows them and uses it -- except a module that
// supplied one of its type arguments, which it follows.
//
// Instances are placed in the order they were made, each once what it depends
// on has a place. Instances that use one another round a loop -- two generic
// modules each instantiating the other at the same arguments -- are placed last,
// in the order made; imports of generic modules are a DAG like any, so that
// loop is the instances' alone.
static void pgmInstanceOrder(ProgramNode *pgm, Nodes *instances) {
    Nodes *pending = newNodes(instances->used);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(instances, cnt, nodesp))
        nodesAdd(&pending, *nodesp);
    while (pending->used > 0) {
        Nodes *waiting = newNodes(pending->used);
        for (nodesFor(pending, cnt, nodesp)) {
            ModuleNode *inst = (ModuleNode*)*nodesp;
            int32_t after = pgmOrderIndex(pgm, (INode*)inst->generic);
            INode **depp;
            uint32_t depcnt;
            for (nodesFor(inst->instdeps, depcnt, depp)) {
                int32_t at = pgmOrderIndex(pgm, *depp);
                if (at < 0) {
                    after = -2;
                    break;
                }
                if (at > after)
                    after = at;
            }
            if (after == -2) {
                nodesAdd(&waiting, (INode*)inst);
                continue;
            }
            // After any instance already placed there, so instances that follow
            // the same module keep the order they were made in
            while ((uint32_t)(after + 1) < pgm->initorder->used
                && ((ModuleNode*)nodesGet(pgm->initorder, after + 1))->generic != NULL)
                ++after;
            nodesInsert(&pgm->initorder, (INode*)inst, after + 1);
            inst->dagmark = 2;
        }
        int progress = waiting->used < pending->used;
        pending = waiting;
        if (!progress) {
            for (nodesFor(pending, cnt, nodesp))
                nodesAdd(&pgm->initorder, *nodesp);
            break;
        }
    }
}

// Type check the program node.
//
// Each instance of a generic module is made, and type checked, where a body
// first names it, while this walk is under way; once it is done, every instance
// joins the program's modules -- so it is generated like any module, in every
// object that uses it -- and takes its place in the init order.
void pgmTypeCheck(TypeCheckState *pstate, ProgramNode *pgm) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        inodeTypeCheckAny(pstate, nodesp);
    }
    Nodes *instances = modInstanceList();
    if (instances == NULL)
        return;
    for (nodesFor(instances, cnt, nodesp))
        nodesAdd(&pgm->modules, *nodesp);
    pgmInstanceOrder(pgm, instances);
}
