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
    pgm->pgmmod = NULL;
    return pgm;
}

// Serialize a program node
void pgmPrint(ProgramNode *pgm) {
    inodeFprint("program:\n");
    inodePrintIncr();
    inodePrintNode((INode*)pgm->pgmmod);
    /*
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        inodePrintIndent();
        inodePrintNode(*nodesp);
        inodePrintNL();
    }*/
    inodePrintDecr();
}

// Name resolution of the program node
void pgmNameRes(NameResState *pstate, ProgramNode *pgm) {
    inodeNameRes(pstate, (INode**)&pgm->pgmmod);
}

// Type check the program node
void pgmTypeCheck(TypeCheckState *pstate, ProgramNode *pgm) {
    inodeTypeCheckAny(pstate, (INode**)&pgm->pgmmod);
}
