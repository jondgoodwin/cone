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
// Today an alias is what a struct's namespace holds for a method, overload set
// or macro method folded in from a field's type by a 'use' clause. Its target is
// a member name use spelling the name in the field's type, bound to the
// declaration when the fold is expanded (structFoldExpand). A folded field is
// not an alias: it is a copy of the field, with a hop (FieldDclNode).
typedef struct AliasDclNode {
    INodeHdr;
    Name *namesym;      // The spelling this binding answers to
    INode *target;      // A name use of what it stands for, bound to that declaration
} AliasDclNode;

// Create an alias under a local spelling for what 'target' names. A folded name
// is public by construction and stands for a member, so both flags are set.
AliasDclNode *newAliasDclNode(Name *namesym, INode *target);

// Clone an alias, mapping the original to the copy for the name uses that follow
INode *cloneAliasDclNode(CloneState *cstate, AliasDclNode *node);

void aliasDclPrint(AliasDclNode *node);

// The declaration at the end of a chain of aliases, or NULL for an alias whose
// target is not yet bound. A node that is not an alias is returned as it is.
INode *aliasDclResolve(INode *node);

#endif
