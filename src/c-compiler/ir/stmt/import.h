/** import statement node and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef import_h
#define import_h

// An import binds another module's name here, and may fold that module's public
// names in beside it. Every binding it makes is an ALIAS carrying a visibility
// of its own -- private to the importing module unless the import is written
// 'pub' -- which is what makes transit fall out of the visibility rule rather
// than being a rule of its own: what a third module sees through this one is
// what this one re-exported.
typedef struct {
    INodeHdr;
    ModuleNode *module;
    FoldClause *fold;   // The names it folds in ('.*' makes a star clause), or NULL for none
} ImportNode;

// Create a new Import node
ImportNode *newImportNode();

void importPrint(ImportNode *pgm);

// Bind the imported module's name in the importing module, as an alias carrying
// this import's visibility. Done at parse, because a later statement in the file
// may qualify a name with it
void importBindModule(ModuleNode *mod, ImportNode *node, uint16_t pubflag);

// Fold the names this import admits into the importing module, as aliases
void importNameRes(NameResState *pstate, ImportNode *mod);

void importTypeCheck(TypeCheckState *pstate, ImportNode *mod);

#endif
