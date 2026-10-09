/** 'each' over something that is not a numeric range, and its lowering
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef each_h
#define each_h

// 'each x in src { body }' over a numeric range is rewritten by the parser
// into a counted loop (parseEach). Over anything else the parser builds a block
// marked FlagEach, whose statements are
//
//   mut <hidden> = src;
//   loop { imm x; ...body... }          // the reader's variables, declared without a value
//
// and type check finishes it (eachLower, from blockTypeCheck, which has checked
// the first statement) once it knows what 'src' is: how the loop walks it is
// the type's to say.
//
// - an array or a slice, or a type that lends an array (a list lends its
//   slice, 'view'), is walked by a counted loop over its length, each variable a
//   borrow of an element: no cursor is made;
// - a type with a 'next' method is a cursor, and is used as it is (a place is
//   advanced in place, a value is held in a variable of the loop's);
// - a type with an 'iter' method gives one, and the loop walks what it gives.
//
// A cursor's 'next' answers an Option, and every pass takes it apart in the
// initializer of the pass's variable, which leaves the loop when there is none:
//
//   imm x = match cursor.next() { case imm s Some { s.value; } case is None { break; } };
//
// A pass's variable is a new, unchangeable variable (not one that the loop
// steps): two or more of them unpack a tuple item, 'imm -item = ...; imm k =
// -item.0; imm v = -item.1'.
void eachLower(TypeCheckState *pstate, BlockNode *outer);

#endif
