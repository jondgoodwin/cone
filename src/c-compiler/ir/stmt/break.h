/** Handling for break nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef break_h
#define break_h

// Node info used by break, return, blockret, and continue statement nodes
// Such statements are only (and must be) found at the end of a loop/regular block.
// We share the same structure across all block-ending statements
// so that we can substitute one for another (return -> break) and to find
// field info in same place (even when not all are populated)
typedef struct {
    INodeHdr;
    INode *exp;         // value returned by break's/return's block (may be 'nil')
    INode *life;        // lifetime for block scope to escape/re-start (null if none specified)
    Nodes *dealias;     // Nodes used to de-alias/unwind current block scope-allocated values
} BreakRetNode;

BreakRetNode *newBreakNode();

// Name resolution for break
void breakNameRes(NameResState *pstate, BreakRetNode *node);

// Clone break
INode *cloneBreakNode(CloneState *cstate, BreakRetNode *node);

void breakTypeCheck(TypeCheckState *pstate, BreakRetNode *node);

typedef struct LoopNode LoopNode;
LoopNode *breakFindLoopNode(TypeCheckState *pstate, INode *life);

#endif
