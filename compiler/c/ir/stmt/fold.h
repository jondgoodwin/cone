/** Name folding at a namespace: the parts a type's fold and a module's share
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fold_h
#define fold_h

// A 'use' clause admits names of one namespace as names of another. Where the
// clause sits decides what the binding holds and what a use of the name lowers
// to, and that part belongs to the site: a field's fold makes a copy carrying a
// hop (struct.c), a sibling's makes a bare alias (struct.c), a global's makes an
// alias reached through the global (here). What every site shares is the clause
// itself -- '*', 'but', a list with 'as' -- and this is that part, lifted here
// when the global's fold became its second user.

// Which members of a source namespace a star clause admits
enum FoldAdmit {
    FoldAdmitMembers,   // Every member reached through a value: a field, a method, a macro method
    FoldAdmitOwn        // Only what the source declares itself: not a field, not an alias of its own
};
// A module's star clause -- an import's, or the one 'extends' makes -- admits by
// rules of its own, pass by pass (importStarAdmits)

// The declaration of a type expression, through a reference or pointer if it is
// one, or NULL when it is not a declaration yet: an instance of a generic still
// to be instantiated, or a name that did not resolve.
INode *foldSourceDcl(INode *vtype);

// Is this name one a clause's 'but' leaves out?
int foldExcluded(FoldClause *fold, Name *name);

// Does the source namespace declare this member itself? A field is the
// representation a sibling already shares, and an alias is what the source
// itself folded in, so neither is the source's own contribution.
int foldAdmitsOwn(INode *member);

// Make the items of a star clause: an alias for every name of 'ns' that 'admit'
// takes and 'but' does not leave out. Reports a 'but' naming what the source
// does not have, which is the one thing a star clause can get wrong by itself.
void foldStarItems(Namespace *ns, Name *srcname, FoldClause *fold, int admit);

// Expand a global's fold clause into its module's namespace, hooking each entry
// when name resolution asks
void foldGlobalExpand(NameResState *pstate, ModuleNode *mod, VarDclNode *global);

// A module's standalone 'use': 'use Colors;', 'pub use Colors Red, Green as
// Verde;', 'use scaling twice, half;'. It names a namespace the module reaches
// WITHOUT an import [Jon 23 Sep] -- an enum, whose variants it folds in and
// nothing else of the enum, or a submodule of this module, reached by its name or
// by a path through submodules, whose public names it folds in exactly as an
// import's clause folds a module's. An imported module is folded by its import's
// own clause and not here: one spelling per situation.
//
// It is a statement of the module rather than a clause on a declaration, so it
// is a node of its own, held on the module's 'moduses' list beside its imports
// and never in its walks: it is neither a field nor a declaration, and what it
// declares is bindings, made in the fold pass.
typedef struct ModUseNode {
    INodeHdr;
    INode *source;      // The enum or submodule, as written: a name or a path, resolved when the fold is expanded
    FoldClause *fold;   // Which names, under which spellings; 'ispub' from 'pub use'
    struct ImportNode *modfold; // Where the source is a submodule: the import-shaped fold over it, sharing 'fold'; else NULL
} ModUseNode;

ModUseNode *newModUseNode();
void modUsePrint(ModUseNode *node);

// Expand a module's standalone 'use' into the module's namespace, hooking each
// name it folds in. Run by modFoldNames in every pass, after the imports and the
// globals: an enum's is made once, and a submodule's is run pass by pass as an
// import's clause is
void foldModUseExpand(NameResState *pstate, ModuleNode *mod, ModUseNode *use);

#endif
