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
	ITypedNodeHdr;
	Nodes *condblk;
} IfNode;

IfNode *newIfNode();
void ifPrint(IfNode *ifnode);
void ifPass(PassState *pstate, IfNode *ifnode);
void ifRemoveReturns(IfNode *ifnode);

#endif