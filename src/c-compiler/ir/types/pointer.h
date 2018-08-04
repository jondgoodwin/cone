/** AST handling for pointer types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef pointer_h
#define pointer_h

// For pointers
typedef struct PtrAstNode {
	NamedTypeAstHdr;
	AstNode *pvtype;	// Value type
	PermAstNode *perm;	// Permission
	AstNode *alloc;		// Allocator
	int16_t scope;		// Lifetime
} PtrAstNode;

PtrAstNode *strType;

PtrAstNode *newPtrTypeNode();
void ptrTypePrint(PtrAstNode *node);
void ptrTypePass(PassState *pstate, PtrAstNode *name);
int ptrTypeEqual(PtrAstNode *node1, PtrAstNode *node2);
int ptrTypeMatches(PtrAstNode *to, PtrAstNode *from);

#endif