/** Alias declaration node: a name in a namespace that stands for another declaration
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef aliasdcl_h
#define aliasdcl_h

// An alias is a binding that stands for another named node: a local spelling
// and a target, nothing else. Its visibility is its own, the FlagPub bit;
// everything else about it is asked of the target, which is what lets a lookup
// that finds an alias act on the declaration in its stead. Aliases chain, and
// aliasDclResolve follows the chain to its end.
//
// A struct's namespace holds one for a method, overload set or macro method
// folded in from a field's type or from a sibling by a 'use' clause. Its target
// is a member name use spelling the name in the source, bound to the declaration
// when the fold is expanded (structFoldExpand). A folded field is not an alias:
// it is a copy of the field, with a hop (FieldDclNode).
//
// A module's namespace holds one for every name a global's 'use' clause folds
// in (foldGlobalExpand), field and method alike, and 'through' is the global.
// There is nothing to copy and no offset to carry, because a global is one
// instance at a fixed address: a use of the name is lowered to 'global.name',
// and everything the hand-written path would do happens from there.
//
// It holds one for a 'typedef' too: there the target is a type expression, which
// name resolution has to reach (FlagTypeAlias), rather than a member name that
// a fold binds.
typedef struct AliasDclNode {
    INodeHdr;
    Name *namesym;      // The spelling this binding answers to
    INode *target;      // A name use of what it stands for, bound to that declaration
    INode *through;     // A name use of the global the target is reached through, or NULL
} AliasDclNode;

// Create an alias under a local spelling for what 'target' names. A folded name
// is public by construction and stands for a member, so both flags are set.
AliasDclNode *newAliasDclNode(Name *namesym, INode *target);

// Create an alias for a name of another namespace, which is what an import's
// fold and an import's own module binding make. It stands for a declaration
// reached with no receiver, and its visibility is its own, so it starts private
// and a re-export is what sets the bit.
AliasDclNode *newNameAliasDclNode(Name *namesym, INode *target);

// Create an alias for a type expression, which is what 'typedef' declares. Its
// visibility is its own, so it starts private and 'pub' is what sets the bit,
// and it stands for a type rather than for a member reached through a receiver.
AliasDclNode *newTypeAliasDclNode(Name *namesym, INode *typeexp);

// Clone an alias, mapping the original to the copy for the name uses that follow
INode *cloneAliasDclNode(CloneState *cstate, AliasDclNode *node);

void aliasDclPrint(AliasDclNode *node);

// Name resolution and type check: only a type alias has anything of its own,
// which is the type expression it stands for
void aliasDclNameRes(NameResState *pstate, AliasDclNode *node);
void aliasDclTypeCheck(TypeCheckState *pstate, AliasDclNode *node);

// Report a chain of type aliases that comes back to itself, and break it so
// that nothing following the chain loops
void aliasDclCheckCycle(AliasDclNode *node);

// The declaration at the end of a chain of aliases, or NULL for an alias whose
// target is not yet bound. A node that is not an alias is returned as it is.
INode *aliasDclResolve(INode *node);

// The global an alias's target is reached through, or NULL for any other
// binding. A use of such a name is lowered to a member access on that global.
INode *aliasDclThrough(INode *node);

// Build that member access: 'global.member', positioned where the name was written
struct FnCallNode *aliasDclThroughAccess(AliasDclNode *node, INode *at);

#endif
