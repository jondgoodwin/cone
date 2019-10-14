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
    INode *life;       // nullable
    Nodes *dealias;
} ContinueNode;

ContinueNode *newContinueNode();

// Name resolution for continue
void continueNameRes(NameResState *pstate, ContinueNode *node);

#endif