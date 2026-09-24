/** Program node helper routines
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new Program node
ProgramNode *newProgramNode() {
    ProgramNode *pgm;
    newNode(pgm, ProgramNode, ProgramTag);
    pgm->modules = newNodes(4);
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

// Name resolution of the program node.
//
// Every module's FOLDED names are put in place first, dependency-first, and only
// then is any module's own body resolved. That order is what stops the file load
// order deciding what a qualified name can reach: a fold used to run at the
// start of the folding module's own resolution, so a module resolved earlier --
// the root among them, since it loads first -- looked the name up before it was
// there, and a module resolved later found it.
//
// Ahead of the folds, what each module's 'extends' names is resolved, and a
// cycle of them refused: the fold follows that edge dependency-first, so every
// edge has to be known, and a cycle cut, before the first fold runs. A cycle of
// imports is not refused: the folds are repeated until they settle (modFoldAll).
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
    for (nodesFor(pgm->modules, cnt, nodesp))
        modExtendsCheckCycle((ModuleNode*)*nodesp, pgm->modules->used);
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
