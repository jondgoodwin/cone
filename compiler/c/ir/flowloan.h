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
// walk cannot tell (a call's result, what a call may store), a loan is both,
// an entry of each kind, which only refuses more; so a loan only near is
// exactly where the value points, held by it nowhere further on, and what is
// read through the value never carries it (pathSetThrough).
//
// An entry may also carry a slot's tag: held by a struct declaring lifetimes
// (lifetime.h), it says in which of them the loan is held -- the struct the
// holder's own type is, or, for a borrowed reference, the one it points at.
// A read of a field carries only the loans of the slots it holds, and those
// with no tag, which may be anywhere. A tag is set only where it is known
// exactly (a struct literal's field, a store into a field, a parameter's
// caller loans), and dropped wherever a value leaves the struct.
#define LoanFar 0x80000000u
#define LoanTagShift 24
#define LoanTagMask (0x1Fu << LoanTagShift)
#define LoanIdMask 0x00FFFFFFu
#define loanOf(entry) ((entry) & LoanIdMask)
#define loanTag(entry) (((entry) & LoanTagMask) >> LoanTagShift)

// The access a borrow with the permission 'perm' makes of what it borrows
int loanBorrowAccess(INode *perm);

// The loan made by the borrow at 'site' of the place 'pl', with the permission
// 'perm'. A site walked again (a loop body) makes the same loan.
uint32_t loanMake(INode *site, Place *pl, INode *perm);

// A caller loan of the parameter 'var': a stand-in for whatever the caller
// lent through it, which no access here conflicts with and which outlives the
// call, one per part of what it lends (LifePart, lifetime.h): what its own
// reference points at, and what that holds, whole or by slot. A parameter
// whose type carries a borrow holds them from the start.
uint32_t loanCaller(uint32_t var, uint32_t part);

// The variable at the root of the place a loan borrows
uint32_t loanRoot(uint32_t loan);

// Does a loan borrow what its root variable points at -- a reborrow through
// a reference, or what a caller lent -- rather than the variable's own storage?
int loanThrough(uint32_t loan);

// Is a loan one of a place reached through the variable 'var' -- of what it
// points at -- by name? Not a caller loan, which stands for what the caller lent
// and is not a name that a store over the variable stales.
int loanNamesThrough(uint32_t loan, uint32_t var);

// Does a loan borrow the whole of its root, or of what its root points at,
// not a part of it?
int loanWhole(uint32_t loan);

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

// Named lifetimes (lifetime.h). A caller loan stands for the lifetimes of the
// part of its parameter it lends: a value may carry it where its type holds
// one they flow to by the signature's order (lifePartFlows). 'sig' is the
// function's own signature.
//
// A caller loan in 'set' whose part flows to no lifetime a value of the type
// 'wanted' holds -- one returned, say -- or 0
uint32_t loanCallerApart(FnSigNode *sig, PathSet *set, INode *wanted);

// A caller loan among 'stored' that may not be stored where a reference
// holding 'refholds' points (its near loans, or every one with 'beyond'):
// what a borrowed parameter points at holds only the lifetimes its type gives
// it there -- in the field the store lands in, where 'landing' names its
// slots, of a struct declaring lifetimes. Returns 0, or the loan, with the
// parameter whose place it may not go in as 'through'.
uint32_t loanStoredApart(FnSigNode *sig, PathSet *stored, PathSet *refholds, int beyond, uint32_t landing,
    VarDclNode **through);

// A loan in 'set' that is not global -- a caller loan, or one of this
// function's own storage -- or 0
uint32_t loanNotGlobalIn(PathSet *set);

// The same, among only its near loans ('near'), its far ones ('far'), or both
uint32_t loanNotGlobalInAs(PathSet *set, int near, int far);

// A far loan in 'set' -- held inside what a value points at -- not known to
// last 'bound' in the signature 'sig': one of this function's own storage,
// or a caller loan of a part whose lifetime its order does not say outlasts
// 'bound'; or 0
uint32_t loanNotBoundIn(FnSigNode *sig, PathSet *set, Name *bound);

// A loan in 'set', near or far, not known to be global in the signature
// 'sig': one of this function's own storage, or a caller loan of a part
// whose lifetime its order does not say outlasts ''static'; or 0
uint32_t loanNotStaticIn(FnSigNode *sig, PathSet *set);

// Report a caller loan whose lifetime may not go where it is carried at
// 'node': returned (LoanEscapeReturn), stored where 'through' points
// (LoanEscapeStore), or handed to a call that may store it there
// (LoanEscapeCall)
void loanApart(INode *node, uint32_t loan, VarDclNode *through, int how);

// Report a loan that is not global handed to a ''static' parameter at 'node',
// or, where 'tparm' names one, for a part the type parameter's ''static'
// bound makes global
void loanNotGlobal(INode *node, uint32_t loan, Name *tparm);

// Report, at 'node', a loan held inside a value stored or returned where a
// virtual reference's bound says what it points at holds lasts 'bound'
void loanNotBound(INode *node, uint32_t loan, Name *bound);

// Report, at 'node', a loan held inside a value converted to an owning
// virtual reference, which holds only global borrows
void loanNotBoxable(INode *node, uint32_t loan);

// The loan the borrow at 'site' made, or 0
uint32_t loanAt(INode *site);

// A seam ('await' in an actor's method; flowpath.c, pwSeam). Every borrow
// that is not global ends there, as a scope ending ends the borrows of what it
// declared: a holder still holding one gets a pending conflict, which fires,
// as the ordinary borrow-no-longer-valid ErrorFrozen pointing at the seam, if
// the holder is used again. A global borrow is ''static', and passes.
//
// Is a loan global: of a global's storage, or a reborrow through a reference
// whose own loans come along with it -- neither a caller loan nor one of this
// function's own storage (loanNotGlobalIn's test)?
int loanIsGlobal(uint32_t loan);

// Does a holder holding 'holds' hold a borrow that is not global? The loan to
// name in a message, the borrow it was given where that can be told, or 0
uint32_t loanSeamEnds(PathSet *holds);

// The pending conflict of the seam 'seam' ending 'loan', which 'holder' holds
uint32_t loanSeamPending(INode *seam, uint32_t loan, uint32_t holder);

// A generator's seam ('yield'; flowpath.c, pwYield) keeps every borrow but one
// of the generator's own ground: of a local, or into the generator itself, where
// the parameters it holds by value are (reached through its own 'self', not past
// a reference it holds). Is this loan such a borrow? The first such loan of a
// set, or 0; and the pending conflict of the seam ending 'loan' for 'holder',
// which fires, as the ordinary borrow-no-longer-valid ErrorFrozen pointing at
// the seam, if the holder is used again
int loanIsGenOwn(uint32_t loan);
uint32_t loanGenOwnIn(PathSet *set);
uint32_t loanYieldPending(INode *seam, uint32_t loan, uint32_t holder);

// The stand-in for the caller of a generator (flowpath.c, pwYield): what a
// 'yield' hands out it holds from there to the end of the body, since the caller
// may keep the value as long as the generator lives. An access or a call that
// conflicts with what it holds is reported where it is made, not at a later use
void loanYieldHolder(uint32_t var);

// The live mark of 'holder' at the seam 'seam': a pending entry that, fired by
// the variable's next use, records it as used after the seam (pathSeamLive),
// and reports nothing. Any variable may carry one, not only a holder.
uint32_t loanSeamLive(INode *seam, uint32_t var);

// Where a lock's guard gives its lock back (flowpath.c, pwLockPoint). The live
// mark of the guard 'guard' at a point after the statement 'site' (kind 0) or
// at the start of the block 'site' (kind 1): a pending entry a use of any
// holder of the guard's borrow fires, on any path from there. A point whose
// mark is never fired has no use of the borrow after it on any path, and is
// where the lock goes back (loanLockStillLive says it was fired).
uint32_t loanLockLive(INode *site, int kind, uint32_t guard);
int loanLockStillLive(uint32_t id);

// Does a loan set hold a loan rooted at the variable 'root'? A set of every
// loan does. Does an operand in flight carry one?
int loanSetRootedAt(PathSet *set, uint32_t root);
int loanFlightRootedAt(uint32_t root);

// The loans in flight across a seam -- an operand already walked whose call
// or value is made after it -- are used after it: one that is not global is
// reported at once, at the operand when it was made by a call or a temporary
// (ErrorAwaitLeftCall: only a plain path is reached again after the seam,
// awaitReReached), and otherwise at the seam
void loanSeamFlight(INode *seam);

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

// The borrow a call returned carries this loan of a place reached through a
// shared path, and the place's container may move what it lent when it
// changes: the loan freezes the place, as a local's would
void loanFreezeShared(uint32_t loan);

// A shape loan: the borrow a call returned carries this loan of a place reached
// through a shared path that another holder may write by, into 'container', a
// struct that changes shape (reshape.h). The loan walk asks of every call made
// while it is held whether the call could reshape a container of that type:
// 'loanShapeCount' loans so far, 'loanShapeId' the i'th. A call that could
// is reported at once when the borrow is in flight ('loanShapeInFlight': an
// operand already walked waits for its call) or handed to the call itself
// (loanShapeNow), and otherwise made a pending conflict for each variable holding
// the loan (loanShapePend), which that variable's next use fires. 'why' is a
// ReshapeWhy, 'callee' the function called, for the message.
int loanExcludesWriters(uint32_t entry);
int loanIsExclusive(uint32_t entry);
void loanShapeMark(uint32_t loan, INode *container);
uint32_t loanShapeCount();
uint32_t loanShapeId(uint32_t i);
INode *loanShapeContainer(uint32_t loan);
int loanShapeInFlight(uint32_t loan);
int loanShapeSamePlace(uint32_t loan, Place *pl);
int loanShapeDisjoint(uint32_t loan, Place *pl);
void loanShapeNow(INode *call, uint32_t loan, int why, Name *callee);
void loanShapePend(INode *call, uint32_t loan, int why, Name *callee);

// Loans in flight: what the walked operands of a call or literal carry, until
// the call is made. A mark to pop back to, and one operand's loans pushed;
// 'reserved' is a two-phase receiver's own loan among them (0 for none).
uint32_t loanFlightMark();
void loanFlightPush(PathSet *carried, uint32_t reserved);
void loanFlightPop(uint32_t mark);
// The same, naming the operand whose loans they are, for a seam's message
void loanFlightPushOf(PathSet *carried, uint32_t reserved, INode *operand);

// An access to a place while loans are in flight: one it conflicts with is
// reported at once (a reserved receiver's loan conflicts as a read-only one)
void loanFlightAccess(Place *pl, int access, INode *node);

// A two-phase receiver's loan activated at its call: reported at 'node' if its
// access conflicts with what another operand pushed since 'mark' carries
void loanFlightActivate(uint32_t mark, uint32_t receiver, int access, INode *node);

// GPU targets (flowGpu). A GPU's pointers are typed by the memory they point
// into, and SPIR-V's logical addressing, and WGSL, cannot choose one at run
// time (no select or phi of pointers), nor keep one in memory. Once every
// function is inlined, each borrow has one origin and its memory kind follows
// from it, so the walk refuses whatever would give a value more than one: a
// reference, or a value holding one, chosen at run time, of any kind. A choice
// is where paths meet and a value's near loans differ between them: an 'if'
// whose arms point at different places, reported at once; or a holder given
// different ones on paths that join, reported at its next use, as a pending
// conflict is, so a holder reassigned before it is used again is not one.
//
// Do the near loans of 'a' and 'b' differ? If so, 'la' is one of 'a''s that
// 'b' lacks, or any of 'a''s, and 'lb' the same of 'b' (0 where a set has none)
int loanNearApart(PathSet *a, PathSet *b, uint32_t *la, uint32_t *lb);

// Report, at 'node', a value of an 'if', a 'match' or a block chosen at run
// time between where 'la' and 'lb' point
void loanChosen(INode *node, uint32_t la, uint32_t lb);

// A pending conflict for the holder 'holder', given where 'la' and 'lb' point
// on paths that joined: fired, as an ErrorGpuRefChoice, at its next use
uint32_t loanChosenPending(uint32_t holder, uint32_t la, uint32_t lb);

// Report, at 'node', an array or slice whose elements hold references indexed
// by a value known only at run time: logical addressing keeps no pointer in
// memory, so such an array must break into separate values, which only a
// literal index allows
void loanIndexedRefs(INode *node);

#endif
