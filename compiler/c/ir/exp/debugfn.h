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
// argschecked: the writer argument is already type checked (a text send's lowering).
int fnCallDebugLower(TypeCheckState *pstate, FnCallNode **nodep, int argschecked);

// Will 'piece.toDebug(out)' be one the compiler writes (a struct, enum, tuple or array
// that declares none)?
int debugWillGenerate(INode *piece);


// The program being type checked, whose generating module holds the functions
// written for a call that stands in a module this compile does not generate
// (core's wrappers, reached from the program's own code)
void debugSetProgram(INode *pgm);

#endif
