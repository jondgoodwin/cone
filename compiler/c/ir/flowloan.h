/** Borrow freezing: the loan walk's client
 * @file
 *
 * A borrow held in a local freezes its source until the borrow's last use. A
 * loan is one borrow of a place; a holder is a variable whose type carries a
 * borrow; an access is anything done to a place. At an access that conflicts
 * with a loan a holder still holds, a pending conflict is recorded against the
 * holder; a later use of the holder fires it (ErrorFrozen), and reassigning the
 * holder, or its leaving scope, drops it. flowpath.c walks the paths and calls
 * these; compiler/c/doc/phases/flow.md, "The loan walk", is the note.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef flowloan_h
#define flowloan_h

// Start one function's walk: its loans and pending conflicts are its own
void loanWalkBegin();

// The loan made by the borrow at 'site' of the place 'pl', with the permission
// 'perm'. A site walked again (a loop body) makes the same loan.
uint32_t loanMake(INode *site, Place *pl, INode *perm);

// An access to a place: each holder that may hold a loan it conflicts with gets
// a pending conflict, which fires if the holder is used again
void loanAccess(Place *pl, int access, INode *node);

// A use of a holder: its pending conflicts fire, each reported once
void loanUse(uint32_t var, INode *usenode);

// A holder now may hold these loans: remember it with each, so an access to a
// loan's place finds the holders to ask
void loanHeldBy(uint32_t var, PathSet *holds);

#endif
