/** Handling for break nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef break_h
#define break_h

// break/continue statement
typedef struct BreakNode {
    INodeHdr;
    INode *life;         // nullable
    INode *exp;          // nullable
    Nodes *dealias;
} BreakNode;

BreakNode *newBreakNode();

// Name resolution for break/continue
// - Ensure it is only used within a while/each block
void breakNameRes(NameResState *pstate, INode *node);

#endif