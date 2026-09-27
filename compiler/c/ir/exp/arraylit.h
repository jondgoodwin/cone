/** Handling for array literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef arraylit_h
#define arraylit_h

// Type check an array literal (used by region allocation only)
void arrayLitTypeCheckDimExp(TypeCheckState *pstate, ArrayNode *arrlit);

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
