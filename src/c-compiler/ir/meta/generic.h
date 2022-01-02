/** Handling for generic nodes (also used for macros)
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef generic_h
#define generic_h

// Create a new generic declaration node
MacroDclNode *newGenericDclNode(Name *namesym);

// Perform generic substitution, if this is a correctly set up generic "fncall"
// Return 1 if done/error needed. Return 0 if not generic or it leaves behind a lit/fncall that needs processing.
int genericSubstitute(TypeCheckState *pstate, FnCallNode **nodep);

#endif
