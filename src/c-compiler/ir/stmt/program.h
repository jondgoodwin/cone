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
    ModuleNode *pgmmod;      // The main program module
} ProgramNode;

ProgramNode *newProgramNode();
void pgmPrint(ProgramNode *pgm);
//void pgmAddNode(ProgramNode *pgm, Name *name, INode *node);
//void pgmAddNamedNode(ProgramNode *mod, Name *name, INode *node);
void pgmNameRes(NameResState *pstate, ProgramNode *mod);
void pgmTypeCheck(TypeCheckState *pstate, ProgramNode *mod);

#endif
