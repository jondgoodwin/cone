/** Borrow freezing: the loan walk's client
 * @file
 *
 * A borrow held in a local freezes its source until the borrow's last use: a
 * source reached as 'uni' (a local, or through 'uni' references) against
 * whatever the borrow's permission forbids; a source reached through a shared
 * path (a 'mut' or 'ro' reference, a 'Rc[mut, T]' owner) only against ending,
 * and against a borrow that would promise more than the path can. A
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

// The access a borrow with the permission 'perm' makes of what it borrows
int loanBorrowAccess(INode *perm);

// The loan made by the borrow at 'site' of the place 'pl', with the permission
// 'perm'. A site walked again (a loop body) makes the same loan.
uint32_t loanMake(INode *site, Place *pl, INode *perm);

// The caller loan of the parameter 'var': a stand-in for whatever the caller
// lent through it, which no access here conflicts with and which outlives the
// call. A parameter whose type carries a borrow holds it from the start.
uint32_t loanCaller(uint32_t var);

// The variable at the root of the place a loan borrows
uint32_t loanRoot(uint32_t loan);

// Is a loan rooted in this function's own storage -- a local, or a by-value
// parameter, or what an owner held in one owns -- so that it ends with the
// call? A caller loan, a loan of a global and a reborrow through a reference
// are not: a reborrow's lifetime is that of the loans the reference held,
// which come along with it.
int loanIsLocal(uint32_t loan);

// A loan in 'set' rooted in this function's own storage, or 0
uint32_t loanLocalIn(PathSet *set);

// May a reference holding 'refholds' point somewhere that outlives this
// function: at what a caller lent, at a global, at what an owner others may
// own too owns ('Rc'), or at nothing the walk knows of? 'referent', the type
// it points at where known (else NULL), sets aside loans of other structs,
// which are held in what it points at rather than pointed at.
int loanMayPointOut(PathSet *refholds, INode *referent);

// A value carrying the local loan 'loan' escapes the function at 'node':
// returned, stored where it may outlive the function, or handed to a call
// that may store it so
enum LoanEscape {
    LoanEscapeReturn,
    LoanEscapeStore,
    LoanEscapeCall,
};
void loanEscape(INode *node, uint32_t loan, int how);

// The loan the borrow at 'site' made, or 0
uint32_t loanAt(INode *site);

// An access to a place: each holder that may hold a loan it conflicts with gets
// a pending conflict, which fires if the holder is used again
void loanAccess(Place *pl, int access, INode *node);

// A use of a holder: its pending conflicts fire, each reported once
void loanUse(uint32_t var, INode *usenode);

// A holder now may hold these loans: remember it with each, so an access to a
// loan's place finds the holders to ask
void loanHeldBy(uint32_t var, PathSet *holds);

// A method's returned borrow carries this loan: name the method in a message
void loanReturnedBy(uint32_t loan, Name *method);

// Loans in flight: what the walked operands of a call or literal carry, until
// the call is made. A mark to pop back to, and one operand's loans pushed;
// 'reserved' is a two-phase receiver's own loan among them (0 for none).
uint32_t loanFlightMark();
void loanFlightPush(PathSet *carried, uint32_t reserved);
void loanFlightPop(uint32_t mark);

// An access to a place while loans are in flight: one it conflicts with is
// reported at once (a reserved receiver's loan conflicts as a read-only one)
void loanFlightAccess(Place *pl, int access, INode *node);

// A two-phase receiver's loan activated at its call: reported at 'node' if its
// access conflicts with what another operand pushed since 'mark' carries
void loanFlightActivate(uint32_t mark, uint32_t receiver, int access, INode *node);

#endif
