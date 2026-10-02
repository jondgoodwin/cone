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

// A value's loans are of two kinds, kept apart in one set. A *near* loan is
// one of a place the value's own borrows point at: what a reference points
// at, what a struct's borrow fields do. A *far* loan is one those places hold
// in turn, a borrow or more further on: borrowing 'q', which holds the
// caller's borrow, points at 'q' (near) and reaches the caller's place only
// through it (far). A store through the reference lands in the near places
// only. An entry is a loan's id, with LoanFar set for a far one. Where the
// walk cannot tell (a value read through a reference, or a call's result),
// every loan is near, which only refuses more.
#define LoanFar 0x80000000u
#define loanOf(entry) ((entry) & ~LoanFar)

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

// Does a loan borrow what its root variable points at -- a reborrow through
// a reference, or what a caller lent -- rather than the variable's own storage?
int loanThrough(uint32_t loan);

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
// own too owns ('Rc'), or at nothing the walk knows of? Only its near loans
// are where it points, unless 'beyond' asks about every place it reaches, a
// borrow or more further on too. 'referent', the type it points at where
// known (else NULL), sets aside loans of other structs, which are held in
// what it points at rather than pointed at.
int loanMayPointOut(PathSet *refholds, INode *referent, int beyond);

// A value carrying the local loan 'loan' escapes the function at 'node':
// returned, stored where it may outlive the function, or handed to a call
// that may store it so; or one carrying the caller loan 'loan' is stored
// into a global
enum LoanEscape {
    LoanEscapeReturn,
    LoanEscapeStore,
    LoanEscapeCall,
};
void loanEscape(INode *node, uint32_t loan, int how);

// Named lifetimes (lifetime.h). A caller loan stands for the lifetimes its
// parameter's type holds: a value may carry it where its type shares one of
// them (lifeShared). The function's own signature is the one compared with.
//
// A caller loan in 'set' whose parameter shares no lifetime with a value of
// the type 'wanted' -- one returned, say -- or 0
uint32_t loanCallerApart(PathSet *set, INode *wanted);

// A caller loan among 'stored' that may not be stored where a reference
// holding 'refholds' points (its near loans, or every one with 'beyond'):
// what a borrowed parameter points at holds only the lifetimes its type gives
// it there. Returns 0, or the loan, with the parameter whose place it may not
// go in as 'through'.
uint32_t loanStoredApart(PathSet *stored, PathSet *refholds, int beyond, VarDclNode **through);

// A loan in 'set' that is not global -- a caller loan, or one of this
// function's own storage -- or 0
uint32_t loanNotGlobalIn(PathSet *set);

// Report a caller loan whose lifetime may not go where it is carried at
// 'node': returned (LoanEscapeReturn), stored where 'through' points
// (LoanEscapeStore), or handed to a call that may store it there
// (LoanEscapeCall)
void loanApart(INode *node, uint32_t loan, VarDclNode *through, int how);

// Report a loan that is not global handed to a ''static' parameter at 'node'
void loanNotGlobal(INode *node, uint32_t loan);

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
