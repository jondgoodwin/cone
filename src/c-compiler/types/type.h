/** Compiler types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef type_h
#define type_h

#include <stdint.h>
typedef struct Symbol Symbol;

// Void type - e.g., for fn with no return value
typedef struct VoidTypeAstNode {
	TypedAstHdr;
} VoidTypeAstNode;

// For pointers
typedef struct PtrTypeAstNode {
	TypedAstHdr;
	unsigned char nbytes;	// e.g., 32-bit uses 4 bytes
	unsigned char subtype;	// Simple, vtabled
	AstNode *ptrtotype;	// Type of value pointer points to
} PtrTypeAstNode;

// For arrays
typedef struct ArrTypeAstNode {
	TypedAstHdr;
	uint32_t nbrelems;		// Number of elements
	AstNode *elemtype;	// Type of array's elements
} ArrTypeAstNode;

// Represents the absence of type information
AstNode *voidType;

void typeInit();
AstNode *typeGetVtype(AstNode *node);
int typeIsSame(AstNode *node1, AstNode *node2);
int typeCoerces(AstNode *to, AstNode **from);

VoidTypeAstNode *newVoidNode();
void voidPrint(VoidTypeAstNode *voidnode);

#endif