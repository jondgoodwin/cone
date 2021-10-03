/** Program structure and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef program_h
#define program_h

// Module is the envelope for all modules for the compiled program
typedef struct {
    INodeHdr;
    Nodes *modules;
} ProgramNode;

ProgramNode *newProgramNode();
void pgmPrint(ProgramNode *pgm);

// Find an already parsed module, or return NULL if not found
ModuleNode *pgmFindMod(ProgramNode *pgm, Name *modname);

// Add a new module to the program
ModuleNode *pgmAddMod(ProgramNode *pgm);

void pgmNameRes(NameResState *pstate, ProgramNode *mod);

void pgmTypeCheck(TypeCheckState *pstate, ProgramNode *mod);

#endif
