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
//
// A module's 'extends' is carried by one too, and never on the module's
// 'imports': it binds no name of its own, and its fold is a star clause over the
// base's declarations and folds, private ones included, each as visible in the
// extending module as it is in the base -- but not over the names the base's
// imports bind to their modules, which are the base's dependencies rather than
// its contents [Jon 23 Sep]. 'isextends' is what tells it apart.
//
// An import inside a module tree may name ANY public name its parent holds, not
// only a module [Jon 23 Sep]: 'import Point;' in a submodule binds the parent's
// public Point the way 'import log;' binds a sister. A submodule is parsed before
// its parent's own files, so what the parent declares is not there to be found
// at parse; the name is held as 'binding', an alias not yet bound, and bound in
// the fold passes once the parent's namespace is complete (importBindName).
// Where the parent's answer is a module -- one the parent imported and
// re-exported -- 'module' is set then, and a 'use' clause folds from it as from
// any module's.
typedef struct ImportNode {
    INodeHdr;
    ModuleNode *module;
    FoldClause *fold;   // The names its 'use' clause folds in ('.*' is 'use *'), or NULL for none
    struct AliasDclNode *binding; // An import of a name of the parent: the alias it binds, bound in the fold passes; else NULL
    uint16_t ispub;     // 'pub import': the module's own binding is public here
    uint16_t isextends; // The fold a module's 'extends' makes, rather than an import statement
    uint16_t isnamedfile; // A bare name reached as a FILE, since the registry held no module of that name at parse
} ImportNode;

// Create a new Import node
ImportNode *newImportNode();

void importPrint(ImportNode *pgm);

// Do two imports of one module say the same thing? The same clause -- the same
// names under the same spellings, the same exclusions, star or not -- and the
// same visibility on each binding. A module imports another once, and a second
// import is refused either way [Jon 23 Sep]; this decides which the diagnostic
// says, the same import written twice or two that disagree.
int importSame(ImportNode *a, ImportNode *b);

// Bind the imported module's name in the importing module, as an alias carrying
// this import's visibility. Done at parse, because a later statement in the file
// may qualify a name with it
void importBindModule(ModuleNode *mod, ImportNode *node);

// Bind the name an import of a name of the parent takes, as far as the parent's
// namespace holds it in this fold pass (modFoldAll); in the pass that reports,
// say why it could not be bound. For an import that reached a FILE by a bare
// name, check in the pass that reports that the registry does not answer that
// name with something else
void importBindName(ModuleNode *mod, ImportNode *node);

// Fold the names this import admits into the importing module, as aliases, as
// far as its module holds them in this fold pass (modFoldAll); in the pass that
// reports, say what could not be folded
void importNameRes(NameResState *pstate, ImportNode *mod);

void importTypeCheck(TypeCheckState *pstate, ImportNode *mod);

#endif
