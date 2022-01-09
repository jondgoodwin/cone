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

// Perform generic substitution, if this is a correctly set up generic "fncall"
// Return 1 if done/error needed. Return 0 if not generic or it leaves behind a lit/fncall that needs processing.
int genericSubstitute(TypeCheckState *pstate, FnCallNode **nodep);

#endif
