/** Handling for array literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef arraylit_h
#define arraylit_h

// Type check an array literal
void arrayLitTypeCheck(TypeCheckState *pstate, ArrayNode *arrlit);

// Is an array actually a literal?
int arrayLitIsLiteral(ArrayNode *node);

#endif
