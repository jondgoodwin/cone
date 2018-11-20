/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef list_h
#define list_h

// Type literal
typedef struct ListNode {
    ITypedNodeHdr;
    INode *type;
    Nodes *elements;
} ListNode;

// Create a new array literal
ListNode *newListNode();
// Serialize an array literal
void listPrint(ListNode *node);
// Check the array literal node
void listWalk(PassState *pstate, ListNode *blk);

#endif