/** The uses of an '@unsized' enum's value in a function's body
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef byvalue_h
#define byvalue_h

// Refuse (ErrorNoSize) every expression in this type-checked function's body
// whose value is an '@unsized' enum, or a tuple or array holding one, unless it
// stands where only its place is asked for: the operand of a borrow, or the base
// of a field access. An '@unsized' enum's variants differ in size, so its value
// can't be copied, assigned, swapped or otherwise held; it is reached only by
// reference.
void byValueCheck(FnDclNode *fn);

#endif
