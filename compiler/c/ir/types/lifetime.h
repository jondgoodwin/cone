/** Named lifetimes on a function's signature
 * @file
 *
 * A borrowed reference type written in a function's signature may name its
 * lifetime, right after the '&': '&'a T', '&'a mut T', '&[]'a u8'. Each name is
 * one lifetime of the caller's, distinct from every other, and a borrow written
 * with none has the unnamed lifetime, one more name, shared by every
 * unannotated borrow in the signature -- and by every borrow a value of another
 * type holds (a struct's field, an 'Option''s element), since a name is written
 * only on a reference. ''static' is the global lifetime: it outlives every name
 * and ties nothing to anything.
 *
 * A name means something only in the signature it is written in, so names are
 * compared by identity, and only between the types of one signature: a
 * callee's own, while its body is checked, or the one a call is made through.
 * No order between two names is ever inferred. RefNode.lifename holds the name
 * (NULL for the unnamed lifetime); RefNode.scope stays the band (0 global, 1 the
 * caller's, 2+ a block of the function), which the names divide no further.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef lifetime_h
#define lifetime_h

struct FnSigNode;

// Does a value of this type hold a borrow of the caller lifetime 'life' (NULL
// for the unnamed one)? A borrow of ''static' is held by nothing in this sense:
// it ties the value to no caller lifetime.
int lifeHolds(INode *type, Name *life);

// Do values of two types of one signature hold borrows of some caller lifetime
// in common?
int lifeShared(INode *a, INode *b);

// What a store through a value of this type lands in: what a borrowed
// reference points at, or else the value itself
INode *lifePointee(INode *type);

// Is this a borrowed reference type whose lifetime is written ''static'?
int lifeIsStatic(INode *type);

// Is ''static' named inside a parameter's type, anywhere but on the parameter's
// own reference? Nothing checks there that a caller's borrow is global.
int lifeParmStaticInside(INode *parmtype);

// Do two signatures promise the same about lifetimes? Each parameter must share
// a lifetime with the result in both or in neither, be ''static' in both or in
// neither, and share one with what each writable borrowed parameter points at
// in both or in neither: those are all a call is checked against.
int lifeSigsAgree(struct FnSigNode *a, struct FnSigNode *b);

// Spell what a signature promises about lifetimes into a type's symbol name
// (nameType), where it differs from what the signature promises unannotated:
// 'G' (where v0 puts a signature's lifetimes), a digit per promise, '_'. Two
// signatures that agree spell alike.
char *lifeSigSpell(char *bufp, struct FnSigNode *sig);

#endif
