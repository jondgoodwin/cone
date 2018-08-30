/** Handling for pointer types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef pointer_h
#define pointer_h

// For pointers
typedef struct PtrNode {
	INodeHdr;
	INode *pvtype;	// Value type
} PtrNode;

PtrNode *strType;

// Create a new pointer type whose info will be filled in afterwards
PtrNode *newPtrNode();

// Serialize a pointer type
void ptrPrint(PtrNode *node);

// Semantically analyze a pointer type
void ptrPass(PassState *pstate, PtrNode *name);

// Compare two pointer signatures to see if they are equivalent
int ptrEqual(PtrNode *node1, PtrNode *node2);

// Will from pointer coerce to a to pointer (we know they are not the same)
int ptrMatches(PtrNode *to, PtrNode *from);

#endif