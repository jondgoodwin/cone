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
    FoldAdmitOwn,       // Only what the source declares itself: not a field, not an alias of its own
    FoldAdmitNames      // Every public name of another MODULE, of whatever kind: a module has one instance, so nothing is reached through a value and nothing is left out for being an alias
};

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

#endif
