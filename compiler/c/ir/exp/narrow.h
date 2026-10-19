/** Flow typing: a name narrowed to one variant of its enum
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef narrow_h
#define narrow_h

// What type check knows of the variant each local or parameter of a closed enum's type
// holds, along the path it is checking: after 'if s is Circle', in its branch; after a
// branch that leaves; by elimination; after 'state = State.Run(2)'. The knowledge is a set
// of the variants the name may hold (a bit for each, in the order of the enum's
// 'derived'): one variant is a narrowing, which type check reads where a member of that
// variant is named on the name ('s.radius', 's.grow()'). It ends at any change of the
// name's variant (narrowKill). A narrowed use is the variable's own, typed as the
// variant; a reference other references may share is narrowed through a hidden local the
// loan walk follows (compiler/c/doc/nodes/narrow.md).
typedef struct NarrowEnv NarrowEnv;

// The environment as it stands, to come back to with narrowRestore, and the one that
// holds where two paths meet. NULL is the empty environment: nothing is known.
NarrowEnv *narrowSave(void);
void narrowRestore(NarrowEnv *env);
NarrowEnv *narrowJoin(NarrowEnv *a, NarrowEnv *b);

// A function body is checked on its own, and in the middle of another's
typedef struct { NarrowEnv *env; int depth; void *frames; uint32_t lazy; void *stmt; uint32_t epoch; void *lastcall; uint32_t tests; } NarrowFnSave;
void narrowFnBegin(NarrowFnSave *save);
void narrowFnEnd(NarrowFnSave *save);

// 'cond', a type checked condition, is known true (or false): what that says of the
// names it tests. The second form is for the right operand of '&&' or '||', where the
// knowledge is good for that operand alone.
void narrowAssume(INode *cond, int truth);
void narrowAssumeLocal(INode *cond, int truth);

// A change to the variable that may change which variant it holds: an assignment to
// the whole of it, a writable borrow of it as its enum, a swap. 'cause' is the node that
// does it.
void narrowKill(VarDclNode *var, INode *cause);

// The changes made so far, so that a branch that leaves the loop can take back what it
// changed: its changes never reach the loop's next pass
uint32_t narrowKillMark(void);
void narrowKillDrop(uint32_t mark);

// 'var = value' was type checked
void narrowAssigned(INode *lval, INode *rval);

// 'var' was declared with a value of a variant of its enum
void narrowDeclared(VarDclNode *var);

// A writable borrow of 'place' (a variable, or the dereference of a reference variable)
// pointing at 'pointee' was made: of the whole enum, it may change the variant
void narrowBorrowed(INode *place, INode *perm, INode *pointee);

// An 'is' test, and a call or a store through a name, was type checked. A name that is a
// reference others may share is narrowed only if no call came between its test and the
// local that holds it (or, within one condition, its use): nothing follows the reference
// before that.
void narrowNoteTest(INode *test);
void narrowCallMade(INode *call);

// A call's receiver, type checked: if a name narrowed to a variant is named on a member
// that only the variant has, the name is typed as the variant
void narrowReceiver(TypeCheckState *pstate, FnCallNode *node);

// The hidden local that holds a shared reference narrowed to a variant is declared where
// the narrowing was made. An 'if' that is a statement of its block (narrowStmtBegin)
// declares it after itself; a branch declares it at its head.
INode *narrowStmtBegin(INode *stmt);
void narrowStmtEnd(INode *prev);
int narrowIsStmt(INode *node);
void narrowAnchorArm(BlockNode *arm, uint16_t scope);
void narrowAnchorAfter(INode *ifnode, uint16_t scope, int direct);
void narrowInsertHeads(BlockNode *arm);
void narrowBlockBegin(BlockNode *blk);
void narrowInsertAfters(BlockNode *blk);   // also: the block is done

// A loop's body is walked once, so a narrowing it inherits from before the loop is
// good for its first pass only: a use of it that a change later in the body reaches by
// the loop's back edge is refused
void narrowLoopEnter(void);
void narrowLoopExit(void);

#endif
