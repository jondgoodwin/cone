/** Handling for type literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef list_h
#define list_h

// Serialize a type literal
void typeLitPrint(FnCallNode *node);
// Name resolution of the literal node
void typeLitNameRes(NameResState *pstate, FnCallNode *lit);
// Check the type literal node
void typeLitTypeCheck(TypeCheckState *pstate, FnCallNode *lit);
// Is the type literal actually a literal?
int typeLitIsLiteral(FnCallNode *node);

#endif