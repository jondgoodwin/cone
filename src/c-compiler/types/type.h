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
	NamedAstHdr;
	unsigned char nbytes;	// e.g., 32-bit uses 4 bytes
	unsigned char subtype;	// Simple, vtabled
	AstNode *ptrtotype;	// Type of value pointer points to
} PtrTypeAstNode;

// For arrays
typedef struct ArrTypeAstNode {
	NamedAstHdr;
	uint32_t nbrelems;		// Number of elements
	AstNode *elemtype;	// Type of array's elements
} ArrTypeAstNode;

// Represents the absence of type information
AstNode *voidType;

void typeInit();
int typeEqual(AstNode *node1, AstNode *node2);

VoidTypeAstNode *newVoidNode();
void voidPrint(int indent, VoidTypeAstNode *voidnode, char *prefix);

#endif