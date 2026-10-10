/** 'each' over something that is not a numeric range, and its lowering; a numeric range's step
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef each_h
#define each_h

// 'each x in a ..< b { body }' over a numeric range is rewritten by the parser
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
//   imm x = match cursor.next() { case imm s Some { s; } case is None { break; } };
//
// A pass's variable is a new, unchangeable variable (not one that the loop
// steps): two or more of them unpack a tuple item, 'imm -item = ...; imm k =
// -item.0; imm v = -item.1'.
//
// A header filter ('each x in src if cond') is the statement 'if !cond {continue}'
// after the variables, built by the parser (parseEachFilterStmt). A loop's 'else'
// is the first statement of the loop, a block flagged FlagLoopElse, which this
// takes out and makes the block the loop leaves through when it has run out.
void eachLower(TypeCheckState *pstate, BlockNode *outer);

// The step of a numeric range ('each x in a ..< b by s') is advanced by the
// parser's own call, on the loop's hidden counter, of this private name
// (eachRangeStepName; its arguments are the range's first value, the count of
// steps taken so far, that count converted to f32 and to f64 ('f32.from(n)', the
// two conversions a float counter may need, written by the parser so that they are
// name resolved) and, when there is one, the step). Type check turns it,
// in fnCallTypeCheck, into the ordinary operator on the counter ('x += s', or
// 'x++' with no step) -- exactly an integer range's step -- or, for a float
// counter, into 'x = first + n * s': a fractional step is computed from the
// first value and the count, never added up, so no rounding builds up in the
// counter (eachRangeStepLower).
extern Name *eachRangeStepName;
void eachRangeNames();
int eachRangeStepLower(TypeCheckState *pstate, FnCallNode **nodep);

#endif
