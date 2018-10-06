/** Handling for return nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef return_h
#define return_h

// Return/yield statement
typedef struct ReturnNode {
	INodeHdr;
    Nodes *dealias;
	INode *exp;
} ReturnNode;

ReturnNode *newReturnNode();
void returnPrint(ReturnNode *node);
void returnPass(PassState *pstate, ReturnNode *node);

#endif