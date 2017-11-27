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

// For primitives such as integer, unsigned integet, floats
typedef struct PrimTypeAstNode {
	NamedAstHdr;
	unsigned char nbytes;	// e.g., int32 uses 4 bytes
} PrimTypeAstNode;

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

// For identifiers that are types rather than values
typedef struct TypeTypeAstNode {
	NamedAstHdr;
	unsigned char subtype;
	AstNode *TypeAstNode;
} TypeTypeAstNode;

// Represents the absence of type information
AstNode *voidType;

// Primitive numeric types
AstNode *i8Type;
AstNode *i16Type;
AstNode *i32Type;
AstNode *i64Type;
AstNode *u8Type;
AstNode *u16Type;
AstNode *u32Type;
AstNode *u64Type;
AstNode *f32Type;
AstNode *f64Type;

void typInit();
VoidTypeAstNode *newVoidNode();
void voidPrint(int indent, VoidTypeAstNode *voidnode, char *prefix);

#endif