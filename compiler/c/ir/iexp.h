/** Expression nodes that return a typed value
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef iexp_h
#define iexp_h

// Castable structure for all typed nodes
typedef struct IExpNode {
    IExpNodeHdr;
} IExpNode;

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *iexpGetTypeDcl(INode *node);

// Return node's type's declaration node
// and when it is a a pointer/ref, get its type declaration node
// (Note: only use after it has been type-checked)
INode *iexpGetDerefTypeDcl(INode *node);

// Type check node we expect to be an expression. Return 0 if not.
int iexpTypeCheckAny(TypeCheckState *pstate, INode **from);

// Return whether it is okay for from expression to be coerced to to-type
TypeCompare iexpMatches(INode **from, INode *totype, SubtypeConstraint constraint);

// A char where a number is wanted, a number where a char is wanted, or a
// non-ASCII character literal wanted as a u8: none reaches the other
// implicitly. Reports it (ErrorCharNotNbr), naming the explicit conversion, and
// answers 1; answers 0, saying nothing, for any other pair of types.
int iexpCharNumberMismatch(INode *from, INode *totypedcl);

// Coerce from-node's type to 'to' expected type, if needed
// Return 1 if type "matches", 0 otherwise
int iexpCoerce(INode **from, INode *totypep);
// Coerce a condition's operand to bool. Only here, and under '!', does an Option
// or Result convert to bool (through isTrue)
int iexpCoerceCondition(INode **from);
// Type check an operand of '!' and coerce it to bool, as a condition
int iexpTypeCheckCoerceCondition(TypeCheckState *pstate, INode **from);
// Is the expression's type Option or Result, or a variant of one?
int iexpIsOptionOrResult(INode *node);

// A 'var' declaration (variable or field) that takes its type from a type checked
// initializer which is a variant of an enum ('var st = Status.Done') holds the
// enum, so it may be given another variant later. The initializer is coerced to the
// enum, and the enum type is the answer. NULL, changing nothing, for any other
// declaration: a 'let' keeps the variant's type, as does a declared type (which
// this is never asked of).
INode *iexpMutDclEnum(INode *perm, INode **valuep);

// The type a value coerced to 'totypedcl' carries: a borrowed reference keeps
// the lifetime (scope) it was borrowed with, and an owner lent as a borrowed
// reference takes the lifetime of a borrow through it, on a copy of the type
// coerced to
INode *iexpCoerceType(INode *from, INode *totypedcl);

// Is this the type of a borrowed reference, whose scope is a lifetime?
int iexpIsBorrowType(INode *type);

// A copy of the borrowed-reference type 'typedcl' carrying the lifetime 'scope'
INode *iexpScopedBorrowType(INode *typedcl, INode *lexnode, uint16_t scope);

// The type of a value that may be any of several, given the narrowest lifetime
// among them: a borrowed-reference type carries that lifetime
INode *iexpNarrowestType(INode *type, INode *lexnode, uint16_t scope);

// Widen 'scope', the narrowest lifetime seen so far, by the value 'exp'
uint16_t iexpNarrowerScope(uint16_t scope, INode *exp);

// Perform full type check on from-node and ensure it is an expression.
// Then coerce from-node's type to 'to' expected type, if needed
// Return 1 if type "matches", 0 otherwise
int iexpTypeCheckCoerce(TypeCheckState *pstate, INode *to, INode **from);

// iexpTypeCheckCoerce for a node already type checked
int iexpCheckedCoerce(INode *to, INode **from);
int iexpCheckedCoerceIn(TypeCheckState *pstate, INode *to, INode **from);

// iexpCoerce where the type check state is at hand, which a string literal lent
// as a temporary needs (slitBorrowCoerce): it is made where a function's body
// wants a read-only borrow of a type that declares 'fromLiteral'
int iexpCoerceIn(TypeCheckState *pstate, INode **from, INode *to);

// Used by 'if' and 'loop' to infer the type in common across all branches,
// one branch at a time. Errors on bad type match and returns Match condition.
// - expectType is the final type expected by receiver
// - maybeType is the inferred type in common
// - from is the current branch whose type is being examined
int iexpMultiInfer(INode *expectType, INode **maybeType, INode **from);

// Used by 'if' and 'block/loop'/break to infer the type in common across all branches,
// one branch at a time. Errors on bad type match and returns Match condition.
// - expectType is the final type expected by receiver (or else don't care)
// - inferredType is the inferred supertype in common
// - fromexp is the current expression node whose type is being examined
// - oldMatch is the current match status on whether all branches match or not
int iexpMultiCoerceInfer(TypeCheckState *pstate, INode *expectType, INode **inferredType, INode **fromexp, int oldMatch);

// iexpMultiCoerceInfer for an expression already type checked
int iexpMultiCheckedCoerceInfer(INode *expectType, INode **inferredType, INode **fromexp, int oldMatch);

// Report that expression 'node' was refused where 'totype' is wanted, as 'code'
// says in 'msg' -- unless the two are function references that differ only in
// the calling convention they are marked for, which ErrorFnRefConv says
void iexpMismatchMsg(INode *node, INode *totype, int code, const char *msg);

// The hint iexpMismatchMsg appends when a plain '&' borrow was given where an
// '&imm' reference is wanted ("; ...write `&imm n`..."), or "" when that is not
// the cause. For a refusal reported without iexpMismatchMsg.
const char *iexpImmBorrowHint(INode *node, INode *totype);

// Return true if an lval, and 0 if not.
int iexpIsLval(INode *lval);

// Ensure it is a lval, return error and 0 if not.
int iexpIsLvalError(INode *lval);

// Is this place reached through a borrowed reference, a slice or a virtual
// reference, so that what is lent of it is held to that loan's permission?
int iexpPathThroughBorrow(INode *lval);

// The type of the first reference the place is reached through that is not
// 'uni' (a '&' or '&mut' borrow, a virtual reference, an owner of another
// permission), or NULL where every one is 'uni' or the place is held by value.
INode *iexpPathNonUniLink(INode *lval);

// Does every reference the place is reached through grant write? The viewpoint
// a borrowed reference held there is seen from: not if one only reads.
int iexpReachWrites(INode *lval);

// Extract lval variable, scope and overall permission from lval. The scope is
// the lifetime a borrow of the place has.
INode *iexpGetLvalInfo(INode *lval, INode **lvalperm, uint16_t *scope);

// The same for a place stored into, whose scope is the lifetime a borrowed
// reference stored there must have at least
INode *iexpGetStoreLvalInfo(INode *lval, INode **lvalperm, uint16_t *scope);

// Are types the same (no coercion)
int iexpSameType(INode *to, INode **from);

// Return true if value uses move semantics
int iexpIsMove(INode *node);

#endif
