/** Could a call change the shape of a collection a borrow points into?
 *
 * A borrow into a shape-changing value (a list, a string, a dictionary: shapeinfer.h)
 * through a shared path is allowed. It is refused only if, while it is alive,
 * something could reshape that value. The same name changing it is the loan
 * walk's own freezing (flowpath.c, pwCall). This is the other half: a change
 * through another name, or a call that might make one. The loan walk asks it of
 * every call made while such a borrow is held or in flight (flowpath.c,
 * pwShapeCall), and reshape.c answers from the types, never from names.
 *
 * A call could reshape a collection of type 'cont' when:
 * - it is a method whose writable 'self' is a 'cont', and the method (in its body,
 *   or what it calls) writes any field of the collection, the header of storage,
 *   runs a finalizer of an element that has one (frees what it owns), or moves
 *   such an element out of the storage and does not put it back (layer 2; layer 1,
 *   for a method whose body is not visible: assumed to). The
 *   receiver's own place is not the shared one when nothing else reaches it: a
 *   local the function owns, or reached through 'uni' references only;
 * - or an argument can reach a value of the type 'cont' (by containment, a field,
 *   an element, a variant, a pointer, a reference, an owner) through a path the
 *   callee may write by, and the callee does not provably leave it alone: its body
 *   is not visible, or it (transitively) reshapes a value of that type that is not
 *   its own local. A reference that is read-only (`&`, `&imm`) cannot write what
 *   it reaches, and a value this function owns whole is nobody else's;
 * - or, handed nothing that reaches the type, it is a callee that reshapes one
 *   through a global.
 *
 * 'clear', 'pop' and 'truncate' each write the length; 'set' of a String finalizes
 * the String it replaces, and 'set' of a number runs nothing. Moving elements about
 * within the storage ('swap', a sort) destroys none and moves none out, and what an
 * element owns stays where it is. An assignment or a swap through another name is no
 * call, and is asked the same (reshapeStoreHits, flowpath.c pwShapeStore): 'm[0] = s'
 * finalizes the element it replaces.
 *
 * An enum is asked the same of a reference narrowed into one of its variants: the
 * borrow is allowed, and refused only if something could change which variant the
 * value holds while the narrowed reference is used (reshapeIsSum). What reshapes an
 * enum is storing over the whole value, or swapping it, by this name or another, in
 * the function or in a callee. Writing a field within the variant does not.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef reshape_h
#define reshape_h

// How a value of one type reaches a value of another (reshapeReach's bits)
enum ReshapeReach {
    ReachContained = 1,     // by containment alone: a field, an element, a variant
    ReachBorrowed = 2,      // through some reference, pointer or owner
    ReachVirtual = 4,       // through a virtual reference, which could be anything
    ReachGaveUp = 8         // the type graph was too large to follow: taken as reaching
};

// Can a value of 'type' reach a value of the struct 'cont'? 'writable' asks only
// of what can be written: a read-only reference (`&`, `&imm`) is not followed.
unsigned reshapeReach(INode *type, INode *cont, int writable);

// Is this place one the function owns whole: a local (or a by-value parameter)
// that is no reference, and a field or array element of one? Nothing else can
// reach it, so it is no other name for a value reached through a shared path.
int reshapeUniqueLocal(INode *place);

// Is this type an enum (or a tagged trait): a value that holds one of several variants?
// A reference narrowed into one of its variants is a borrow into it, and what reshapes
// it is a change of which variant it holds: the whole value stored over, or swapped,
// through another name. Writing a field within the variant is not a reshape.
int reshapeIsSum(INode *type);

// Does storing over the place 'lval' reshape a value of 'cont'? A store of the type
// itself replaces one whole (an enum then may hold another variant; a collection
// frees its block), and so does a store of a value that holds one by value (a struct
// with a list or an enum for a field). A store over an element of a collection, found
// by indexing it, that owns something (a String, an element of a 'List[String]')
// frees what the old one owned: a borrow of its bytes dangles. A number stored, any
// element written that owns nothing, and a field of a struct that merely holds a String
// reshape nothing.
int reshapeStoreHits(INode *lval, INode *cont);

// A global (a module-level variable, or a static) has been type checked. Whether
// any global can reach a collection type decides whether a call that is handed
// nothing reaching it still needs asking about
void reshapeNoteGlobal(VarDclNode *var);
int reshapeGlobalsReach(INode *cont);

// Why a call could reshape
enum ReshapeWhy {
    ReshapeNot,
    ReshapeReceiver,    // a method taking the collection writable, whose body writes a header field
    ReshapeReceiverUnseen,  // the same, a method whose body the compiler cannot see: layer 1 assumes it does
    ReshapeUnseen,      // a callee whose body the compiler cannot see, handed a writable value that can reach it
    ReshapeBody,        // a callee handed a writable value that can reach it, whose body reshapes one
    ReshapeGlobal,      // a callee that reshapes one through a global, handed nothing that reaches it
    ReshapeStore        // a store through another name (not a call): reshapeStoreHits
};

typedef struct {
    uint8_t why;            // ReshapeWhy
    FnDclNode *callee;      // the function called, when known
} ReshapeVerdict;

// Could this call reshape a value of the struct 'cont'? 'meth' is the method it
// calls when its first argument is the receiver (else NULL); 'recvunique' says
// the receiver's place is reached by no shared path, so no other name for it
// exists. Fills 'verdict' when it answers yes.
int reshapeCall(FnCallNode *call, FnDclNode *meth, int recvunique, INode *cont, ReshapeVerdict *verdict);

#endif
