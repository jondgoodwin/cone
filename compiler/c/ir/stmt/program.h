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
    Nodes *initorder;   // Every module, each after every module it depends on (pgmModuleOrder): the order 'init' runs in
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

// Put the program's modules in DEPENDENCY ORDER, refusing a loop [Jon 23 Sep].
// Imports form a DAG at every scale, sister modules as well as packages. A module
// depends on each module it imports, on the module holding a name it imports
// ('import Point;' in a submodule depends on its parent), on the module it
// extends, and on each of its own submodules (containment): so a child that
// depends on its parent closes a two-module loop. One walk, once what 'extends'
// names is known; each loop is ErrorImportLoop, naming the modules round it, and
// the result is pgm->initorder, dependencies first -- the order module 'init's
// will run in
void pgmModuleOrder(ProgramNode *pgm);

void pgmNameRes(NameResState *pstate, ProgramNode *mod);

void pgmTypeCheck(TypeCheckState *pstate, ProgramNode *mod);

#endif
