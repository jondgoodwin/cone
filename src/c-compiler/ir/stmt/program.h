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

// Find the module already loaded from a file of this name, or NULL. The key is
// the filename-derived name, never the module's declared one: loading twice is
// what must not happen, and a 'mod' declaration may name the module anything
ModuleNode *pgmFindModFile(ProgramNode *pgm, Name *filesym);

// Add a new module to the program
ModuleNode *pgmAddMod(ProgramNode *pgm, int16_t flags);

void pgmNameRes(NameResState *pstate, ProgramNode *mod);

void pgmTypeCheck(TypeCheckState *pstate, ProgramNode *mod);

#endif
