/** Handling for deref nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef deref_h
#define deref_h

// The definition of StarNode etc. is in pointer.h

void derefPrint(StarNode *node);

// Type check deref node
void derefTypeCheck(TypeCheckState *pstate, StarNode *node);

// Inject automatic deref node, if node's type is a ref or ptr. Return 1 if dereffed.
int derefInject(INode **node);

// Is this the dereference that makes a plain index through a type's '[]' a
// place (FlagIndexPlace)?
#define derefIsIndexPlace(node) ((node)->tag == DerefTag && ((node)->flags & FlagIndexPlace))

// '--warn-index-star': warn at each explicit '*x[i]' over an index place, the
// transitional spelling (silent by default; a migration's site list)
extern int derefWarnIndexStar;

// Perform data flow analysis on deref node
void derefFlow(FlowState *fstate, StarNode **node);

#endif
