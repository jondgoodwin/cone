/** AST handling for pointer types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef pointer_h
#define pointer_h

// For pointers
typedef struct PtrTypeAstNode {
	TypeAstHdr;
	AstNode *pvtype;	// Value type
	PermAstNode *perm;	// Permission
	AstNode *alloc;		// Allocator
	int16_t scope;		// Lifetime
} PtrTypeAstNode;

PtrTypeAstNode *newPtrTypeNode();
void ptrTypePrint(PtrTypeAstNode *node);
void ptrTypePass(AstPass *pstate, PtrTypeAstNode *name);
int ptrTypeEqual(PtrTypeAstNode *node1, PtrTypeAstNode *node2);

#endif