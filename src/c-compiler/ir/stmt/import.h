/** import statement node and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef import_h
#define import_h

// Module is the envelope for all modules for the compiled program
typedef struct {
    INodeHdr;
    ModuleNode *module;
    int foldall;   // was "*" specified?
} ImportNode;

void importPrint(ImportNode *pgm);

void importNameRes(NameResState *pstate, ImportNode *mod);

void importTypeCheck(TypeCheckState *pstate, ImportNode *mod);

#endif
