/** A statement that throws a value away
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef discard_h
#define discard_h

// A statement 'f();' whose expression has a value drops it. Most values are
// fine to drop -- 'list.push(x)' hands the list back for chaining -- but a
// failure nobody reads is how errors go missing, so WarnDiscard is raised when
// the dropped value is
// - a value of a struct, enum or variant declared '@mustUse', or of a variant
//   of an enum so declared. Core's Result is one, whatever its generic
//   arguments, and where a generic's T became one (the statement is what the
//   reader wrote, and what it drops is the instance's type). An Option is not:
//   a pop, an insert or a remove that finds nothing is not a failure unread;
// - the answer of an 'await' on an operation (an actors.Awaitable: an I/O
//   operation), whatever type that is, which says whether it worked.
// The reader says the drop is deliberate with '_ = f();', which is an
// assignment and so not a statement that drops anything. A warning never fails
// a build.
//
// Only blockStmtTypeCheck asks, for a statement of a block: the parser has
// already made a block's last line carry no ';' where its value is used and a
// ';' where it is not (ErrorTailOpen), so every dropped value stands as a
// statement followed by another. A reference to a Result is no Result, and a
// statement that is a declaration, assignment or jump is no value dropped.
void discardCheck(INode *stmt);

#endif
