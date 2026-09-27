/** Handling for borrow expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef borrow_h
#define borrow_h

// Uses RefNode defined in reference.h

// Create a borrowed ref node
INode *newBorrowMutRef(INode *node, INode* type, INode *perm);

// Inject a borrow mutable node on some node (expected to be an lval)
void borrowMutRef(INode **node, INode* type, INode *perm);

// Auto-inject a borrow note in front of 'from', to create totypedcl type
void borrowAuto(INode **from, INode *totypedcl);

// Can we safely auto-borrow to match expected type?
// Note: totype has already done GetTypeDcl
int borrowAutoMatches(INode *from, RefNode *totype);

// Is a borrow's operand a literal kept in a constant global: a string literal,
// an array literal of constants, or a named constant holding either?
int borrowIsConstLit(INode *node);

// Retype a borrowed constant array literal to the reference type it is wanted as
int borrowConstLitCoerce(INode *from, INode *totypedcl);

// Is 'from' a '&uni' reference in a place, wanted as a shareable borrowed reference?
int borrowUniReborrows(INode *from, INode *totypedcl);

// Lend such a '&uni' reference by rewriting it to the reborrow '&mut *from'
void borrowUniReborrow(INode **from, INode *totypedcl);

// Is 'from' a sole owning reference in a place, wanted as a '&uni' borrowed reference?
int borrowOwnerLendsUni(INode *from, INode *totypedcl);

// Lend such an owning reference by rewriting it to the borrow '&uni *from'
void borrowOwnerLend(INode **from, INode *totypedcl);

void borrowPrint(RefNode *node);

// Type check borrow node
void borrowTypeCheck(TypeCheckState *pstate, RefNode **node);

// Perform data flow analysis on addr node
void borrowFlow(FlowState *fstate, RefNode **nodep);

#endif
