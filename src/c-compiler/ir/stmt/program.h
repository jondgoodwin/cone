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
    Namespace files;    // The file registry: every source file read, by its path
} ProgramNode;

ProgramNode *newProgramNode();
void pgmPrint(ProgramNode *pgm);

// The module a file already belongs to, or NULL. The key is the file's path, so
// a file is read exactly once and belongs to exactly one module however many
// importers name it; neither the module's declared name nor its filename is the
// key, because what must happen once is the reading of the file.
ModuleNode *pgmFindFile(ProgramNode *pgm, Name *pathsym);

// Record that a file belongs to a module
void pgmSetFile(ProgramNode *pgm, Name *pathsym, ModuleNode *mod);

// Add a new module to the program
ModuleNode *pgmAddMod(ProgramNode *pgm, int16_t flags);

void pgmNameRes(NameResState *pstate, ProgramNode *mod);

void pgmTypeCheck(TypeCheckState *pstate, ProgramNode *mod);

#endif
