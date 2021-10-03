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
    return pgm;
}

// Add a new module to the program
ModuleNode *pgmAddMod(ProgramNode *pgm) {
    ModuleNode *mod = newModuleNode();
    nodesAdd(&pgm->modules, (INode*)mod);
    return mod;
}

// Find an already parsed module, or return NULL if not found
ModuleNode *pgmFindMod(ProgramNode *pgm, Name *modname) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(pgm->modules, cnt, nodesp)) {
        ModuleNode *mod = (ModuleNode *)*nodesp;
        if (mod->namesym == modname)
            return mod;
    }
    return NULL;
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

// Name resolution of the program node
void pgmNameRes(NameResState *pstate, ProgramNode *pgm) {
    INode **nodesp;
    uint32_t cnt;
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
