/** Handling for array literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef arraylit_h
#define arraylit_h

// An array's contents repeating a value write out a copy of it for each
// element up to this many; beyond, one copy is generated in a loop
// (contentsArrayLit), and a fill form's constant is stored by a loop or a
// memset rather than as one aggregate (genlArrayLitInto)
#define ArrayRepeatUnroll 16

// Type check an array literal, against the type expected of it if any
void arrayLitTypeCheck(TypeCheckState *pstate, ArrayNode *arrlit, INode *expectType);

// Coerce an array literal to an array type of the same size, element by element.
// Return 1 if every element coerces, 0 otherwise.
int arrayLitCoerce(ArrayNode *arrlit, INode *totypedcl);

// Perform data flow analysis on an array literal's element values
void arrayLitFlow(FlowState *fstate, ArrayNode **nodep);

// Is an array actually a literal?
int arrayLitIsLiteral(ArrayNode *node);

#endif
