/** Handling for return nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef return_h
#define return_h

// Return statements use the BreakRetNode structure defined in break.h

BreakRetNode *newReturnNode();
BreakRetNode *newReturnNodeExp(INode *exp);

// Clone return
INode *cloneReturnNode(CloneState *cstate, BreakRetNode *node);

void returnPrint(BreakRetNode *node);
// Name resolution for return
void returnNameRes(NameResState *pstate, BreakRetNode *node);

// Type check for return statement
// Related analysis for return elsewhere:
// - Block ensures that return can only appear at end of block
// - NameDcl turns fn block's final expression into an implicit return
void returnTypeCheck(TypeCheckState *pstate, BreakRetNode *node);

#endif
