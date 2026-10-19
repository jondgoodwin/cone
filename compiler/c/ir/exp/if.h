/** Handling for if nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef if_h
#define if_h

// If statement
typedef struct IfNode {
    IExpNodeHdr;
    Nodes *condblk;
} IfNode;

// An 'if' that a 'match' was de-sugared into: the arms are the cases of one match,
// so type check asks that they cover every value (ifMatchCover)
#define FlagIfMatch   0x0001        // If
// The 'if' that 'let Ok(n) = r else {...};' is de-sugared into: its 'else' must leave,
// since the binding that follows it takes the variant the test found
#define FlagIfLetElse 0x0002        // If
// The cases of a match after one that binds with a function's pattern (parseBoundMatch): the
// chain goes on in an 'else' block that holds the function's result, so these are an 'if' of
// their own. The match's coverage is judged on the first (FlagIfMatch), which ends in that 'else'.
#define FlagIfMatchTail 0x0004      // If
// A match with a case that uses a function's pattern, which is required to end in 'else'
// whether or not its other cases cover every value, so an 'else' that cannot run is no error
#define FlagIfMatchUsing 0x0008     // If
#define MatchMaxColumns 8           // the values a 'match a, b, ...' may take
// The logic nodes of a match's conditions: the '&&' that joins the patterns of one
// row of 'match a, b' (one per column), and the '&&' that adds an 'if' guard to the
// pattern before it. A '&&' of neither kind is a range pattern.
#define FlagMatchRow  0x0001        // Logic: joins the columns of a row
#define FlagMatchGuard 0x0002       // Logic: a guard after the pattern
// A literal 'true' that stands for '_', a column's pattern that matches any value
#define FlagMatchAny  0x0008        // ULit

IfNode *newIfNode();

// Clone if
INode *cloneIfNode(CloneState *cstate, IfNode *node);

void ifPrint(IfNode *ifnode);

// if node name resolution
void ifNameRes(NameResState *pstate, IfNode *ifnode);

// Type check the if statement node
// - Every conditional expression must be a bool
// - if's vtype is specified/checked only when coerced by iexpCoerces
void ifTypeCheck(TypeCheckState *pstate, IfNode *ifnode, INode *expectType);

void ifRemoveReturns(IfNode *ifnode);

// Does a type-checked block end by jumping away -- a 'return', 'break' or
// 'continue', a call that does not return among them (blockTypeCheck) -- so
// that it gives its 'if' no value?
int ifBlockJumps(BlockNode *blk);

// Does every path through a type-checked 'if' jump away: it has an 'else',
// and each of its blocks does?
int ifAllPathsJump(IfNode *ifnode);

// Perform data flow analysis on an if expression
void ifFlow(FlowState *fstate, IfNode **ifnodep);

#endif
