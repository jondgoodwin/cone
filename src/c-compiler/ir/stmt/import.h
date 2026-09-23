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
// names in beside it with a 'use' clause. Every binding it makes is an ALIAS
// carrying a visibility of its own -- private to the importing module unless the
// import says 'pub' -- which is what makes transit fall out of the visibility
// rule rather than being a rule of its own: what a third module sees through
// this one is what this one re-exported.
//
// Two spellings of 'pub', and they cannot disagree: 'pub import' makes every
// binding the import makes public, the module's name and each fold alike, and
// 'pub use' makes only the folds public. So 'ispub' is the module binding's bit,
// and the clause's 'ispub' is set by either spelling.
typedef struct {
    INodeHdr;
    ModuleNode *module;
    FoldClause *fold;   // The names its 'use' clause folds in ('.*' is 'use *'), or NULL for none
    Nodes *cycle;       // Set when the module's folds were still running as this import read it: the imports round that cycle, the module's own first and this one last
    uint16_t ispub;     // 'pub import': the module's own binding is public here
} ImportNode;

// Create a new Import node
ImportNode *newImportNode();

void importPrint(ImportNode *pgm);

// Do two imports of one module say the same thing? The same clause -- the same
// names under the same spellings, the same exclusions, star or not -- and the
// same visibility on each binding. A module imports another once: an identical
// repeat is ignored, and one that differs is refused.
int importSame(ImportNode *a, ImportNode *b);

// Bind the imported module's name in the importing module, as an alias carrying
// this import's visibility. Done at parse, because a later statement in the file
// may qualify a name with it
void importBindModule(ModuleNode *mod, ImportNode *node);

// Fold the names this import admits into the importing module, as aliases
void importNameRes(NameResState *pstate, ImportNode *mod);

void importTypeCheck(TypeCheckState *pstate, ImportNode *mod);

#endif
