/** Handling for generic nodes (also used for macros)
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef generic_h
#define generic_h

// Create a new generic declaration node
MacroDclNode *newGenericDclNode(Name *namesym);

// Instantiate a generic using passed arguments
void genericCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep);

// Instantiate a generic function node whose type parameters are inferred from the function call arguments
int genericInferVars(TypeCheckState *pstate, FnCallNode **nodep);

#endif
