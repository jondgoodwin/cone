/** Drop flags: the path walk's second client
 * @file
 *
 * Whether a variable holds its value at a scope's end, or when it is stored
 * over, may differ by path: moved out on one branch, given a value on one,
 * moved out in an earlier pass of a loop. The path walk (flowpath.c) follows
 * what each tracked variable may hold -- its whole value, a hollowed one,
 * nothing -- along every path, and asks this client to record it at each
 * release: a scope's exits, and each assignment. Where every path agrees, the
 * release is what it is or nothing; where they differ, the variable gets a drop
 * flag, a hidden byte generation keeps up to date where its value arrives or
 * leaves, and the release is guarded by it. The walk also refuses a use of a
 * value some path reaching it has moved out (a loop's earlier pass) or never
 * gave one. compiler/c/doc/phases/flow.md, "Drop flags", is the note.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef flowdrop_h
#define flowdrop_h

// Start one function's walk: its records are its own
void dropWalkBegin();

// A marked name use (FlagMoveOut, FlagHollowOut) of the variable at 'var' --
// the owner of the value, for a match's binding: the value leaves here
void dropMove(uint32_t var, INode *node, int hollow);

// A use of the variable at 'var' that needs its value: a read (or, with
// 'borrow', a borrow, which needs it only not moved out)
void dropUse(uint32_t var, INode *node, int borrow);

// The variable at 'var' is stored over whole ('lval' names it; 'rvalp' is the
// value's slot, NULL where the value is one element of another), or in part
void dropStore(uint32_t var, INode *lval, INode **rvalp);
void dropPartStore(uint32_t var, INode *lval);

// A scope's exit, releasing the 'n' variables at 'vars' (indexes, in order of
// declaration) as it leaves
void dropExit(INode *exit, uint32_t *vars, uint32_t n);

// The walk is done: 'ok' when it reported no error. Give each variable whose
// state differs at a release its flag, and rebuild each release the walk met.
void dropWalkEnd(int ok);

// -V 2: functions walked for drops, and variables given a flag
void dropPrint();

#endif
