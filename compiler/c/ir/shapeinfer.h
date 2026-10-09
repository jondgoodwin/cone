/** Which types change shape, read from their methods
 *
 * A borrow of an element of a value that may change shape (a list, a string, a
 * dictionary) points into storage that a change can move or free. The loan walk
 * freezes the path such a borrow was reached through, even a shared one
 * (flowpath.c, pwCall). This says which types those are.
 *
 * A type declaring none of 'ShapeChanging', 'NoLoanMut' and 'NoLoanRead' is
 * shape-changing when BOTH hold of its methods:
 * - one of them takes a writable 'self' and, in its own body or in anything it
 *   calls, writes a place that holds storage: a raw pointer, or a value holding
 *   one or an owning reference (a pointer field, or a field that is itself a
 *   list); or hands such a place's pointer to code the compiler cannot see; and
 * - one of them returns a borrow (itypeCarriesBorrow).
 * Writing a number field (a cursor advancing, a count) reshapes no storage, and
 * writing an element through the pointer moves nothing.
 *
 * Code the compiler cannot see counts as writing: an 'extern' function, a
 * virtual call, a function pointer, and a function of another package whose
 * body its include file left out. That last case is decided once, where the
 * body is visible: a package's include file records each of its types the
 * compiler found shape-changing by writing 'ShapeChanging' into the type's 'is'
 * list (incfile.c), and an importer takes a type from an include file as that
 * file says, found or not.
 *
 * A type may declare 'ShapeChanging' itself. That only asserts what the compiler
 * finds: a type declaring it that the compiler does not find so is refused
 * (ErrorShapeMark), so the declaration and the methods cannot drift apart.
 *
 * The answer is read from the bodies of the type's methods, and type check is
 * demand-driven, so a body may not be checked yet when the loan walk of another
 * function asks. The walk then waits (shapeWalkReady, shapeWalkDefer), and is
 * made at the end of type check (shapeWalkDeferred).
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef shapeinfer_h
#define shapeinfer_h

// Is a value of this type taken to change shape: declared so, or found so?
// An answer not settled yet is taken as yes.
int shapeChanging(StructNode *st);

// Called before the loan walk of 'fn': settle what the walk will ask, checking
// the methods of the types it meets that are waiting to be (a demand, outside any
// walk). 0 where something cannot be settled yet and 'maydefer' says the walk
// may wait: the caller then queues it with shapeWalkDefer. 1: walk now, an answer
// still unsettled being taken as yes.
int shapeWalkReady(FnDclNode *fn, int maydefer);

// Queue the loan walk of 'fn', with its drop walk when 'drops'
void shapeWalkDefer(FnDclNode *fn, int drops);

// The end of type check: make every queued walk
void shapeWalkDeferred(void);

// A struct declaring 'ShapeChanging' has been laid out: it is checked against
// what the compiler finds, at the end of type check
void shapeDeclared(StructNode *st);
void shapeDeclaredCheck(void);

// Does the package's include file say this type is shape-changing, though its
// source does not? True of a type of the package the compiler found so
int shapeRecordable(StructNode *st);

#endif
