/** Handling for continue nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef continue_h
#define continue_h

// Continue statements use the BreakRetNode structure defined in break.h

BreakRetNode *newContinueNode();

// Clone continue
INode *cloneContinueNode(CloneState *cstate, BreakRetNode *node);

// Name resolution for continue
void continueNameRes(NameResState *pstate, BreakRetNode *node);
void continueTypeCheck(TypeCheckState *pstate, BreakRetNode *node);

#endif
