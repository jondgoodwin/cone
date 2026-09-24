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

// Type check the program node
void pgmTypeCheck(TypeCheckState *pstate, ProgramNode *pgm) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        inodeTypeCheckAny(pstate, nodesp);
    }
}
