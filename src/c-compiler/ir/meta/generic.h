/** Handling for generic nodes (also used for macros)
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef generic_h
#define generic_h

typedef struct GenericInfo {
    Nodes *parms;            // Declared parameter nodes w/ defaults (GenVarTag)
    Nodes *memonodes;        // Pairs of memoized generic calls and cloned bodies
} GenericInfo;

// Create a new generic info block
GenericInfo *newGenericInfo();

// Serialize
void genericInfoPrint(GenericInfo *info);

// Obtain the GenericInfo a declaration carries, or NULL if it is not a generic.
// This is what distinguishes a generic from every other declaration: a generic
// is an ordinary FnDcl or StructNode with a parameter list attached, and not a
// declaration node of its own.
GenericInfo *genericGetInfo(INode *node);

// Bound the nesting of generic instantiation and macro expansion.
//
// Both work by cloning a declaration and analyzing the clone, and analyzing it
// can expand the same declaration again at larger arguments. Every expansion is
// a distinct node, so the Analyzing mark never sees such a cycle -- nothing ever
// returns to the same node. Depth is the only thing that tells a generic that
// terminates from one that does not, and past the limit it is the C stack the
// walk runs on that gives out, with no diagnostic at all.
//
// Enter reports and returns 0 when the limit is reached; the caller substitutes
// an error node for what it could not expand. Every successful Enter is paired
// with an Exit once the expansion has been analyzed.
int genericInstantiateEnter(INode *errnode);
void genericInstantiateExit();

// Perform generic substitution, if this is a correctly set up generic "fncall"
// Return 1 if done/error needed. Return 0 if not generic or it leaves behind a lit/fncall that needs processing.
int genericSubstitute(AnalysisState *pstate, FnCallNode **nodep);

#endif
