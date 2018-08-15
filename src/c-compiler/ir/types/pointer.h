/** AST handling for pointer types
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
	PermNode *perm;	// Permission
	INode *alloc;		// Allocator
	int16_t scope;		// Lifetime
} PtrNode;

PtrNode *strType;

PtrNode *newPtrTypeNode();
void ptrTypePrint(PtrNode *node);
void ptrTypePass(PassState *pstate, PtrNode *name);
int ptrTypeEqual(PtrNode *node1, PtrNode *node2);
int ptrTypeMatches(PtrNode *to, PtrNode *from);

#endif