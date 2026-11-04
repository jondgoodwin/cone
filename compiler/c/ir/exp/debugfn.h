/** The debug form of a value, generated on use
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef debugfn_h
#define debugfn_h

// A call 'x.toDebug(out)' whose receiver's type declares no 'toDebug': a number
// is written as its text, a char through the writer's putDebugChar, and any
// other struct, enum or tuple through a function the compiler writes for the
// type, once per module, when the call asks (debugfn.c). Answers 1 when it
// replaced the call, type checked; 0 to go on with the call as it is (possibly
// retargeted to the method that writes it).
int fnCallDebugLower(TypeCheckState *pstate, FnCallNode **nodep);

// Is this function one the compiler wrote for a type's debug form? It reads the
// type's private fields, as the type's own methods do.
int debugFnIsGenerated(INode *fn);

#endif
