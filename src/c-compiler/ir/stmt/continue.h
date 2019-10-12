/** Handling for continue nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef continue_h
#define continue_h

// continue statement
typedef struct ContinueNode {
    INodeHdr;
    LifetimeNode *life;   // nullable
    Nodes *dealias;
} ContinueNode;

ContinueNode *newContinueNode();

// Name resolution for continue
// - Ensure it is only used within a while/each/loop block
void continueNameRes(NameResState *pstate, INode *node);

#endif